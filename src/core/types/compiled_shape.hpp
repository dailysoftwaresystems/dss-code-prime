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

// FC4 c1 (M4 — the D2a guard-polarity decision, C11 6.7.6.3p11): how the
// type-name commit triage treats a lone identifier the binder sketch has NO
// entry for (triage rule 4). KNOWN names are polarity-independent (Type →
// commit, Value → rollback), as is the rule-1 multi-leaf commit (a
// keyword-led type child cannot be an expression).
//
//   PreferType       — the FC2 default: commit iff the follower token could
//                      not continue a value reading (not an infix/postfix/
//                      ternary operator per the operator table); an
//                      operator follower rolls back + records the
//                      AmbiguousTypeNameCandidate for the CU oracle.
//   RequireKnownType — commit ONLY on a sketch-KNOWN type. Unknown always
//                      rolls back to the competing reading AND records the
//                      candidate (so a cross-file typedef still resolves on
//                      the oracle's seeded reparse). The C 6.7.6.3p11
//                      parameter-position rule: `T (name)` is a parenthesized
//                      declarator unless `name` is a visible typedef.
enum class TypeNameCommitPolarity : std::uint8_t {
    PreferType,
    RequireKnownType,
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
    // The loader's `recomputeAltExpectedSets` fixed-point: an AltChoice's
    // expectedSet is the union of its branches', but `repeat`'s tie-the-knot
    // builds the loop body BEFORE the loop-entry AltChoice exists, so an
    // optional/alt INSIDE the body captures a stale-empty expectedSet for the
    // loop-back branch. The post-build fixed-point re-unions to convergence.
    void setExpectedSet(std::vector<SchemaTokenId> v) noexcept {
        expectedSet_ = std::move(v);
    }
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
    std::vector<SchemaTokenId> followSet;
    bool                       nullable = false;

    // `expr`-shape metadata. `isExpr` true iff the rule's body is
    // `{ "expr": { "atom": ... } }`. The cursor still compiles `expr`
    // as a transparent reference to the atom rule (see
    // `PositionBuilder::build`); these fields tell the parser to
    // hand off to a Pratt walker instead of recursing through the
    // cursor's atom RuleLeaf.
    bool                       isExpr = false;
    RuleId                     exprAtom{};
    std::int32_t               exprMinPrecedence = 0;

    // Type-name commit guard (`commitRequiresTypeName` on the shape body;
    // FC2 cast-expression disambiguation). When valid, names the child rule
    // occupying this rule's TYPE position: a structurally-successful
    // speculative probe of this rule COMMITS only after the parser's
    // generic type-name triage over that child's subtree (a lone
    // `identifierToken` leaf consults the binder sketch / the
    // operator-table follower test; any other form — keyword base,
    // struct-tag, pointer star, const — commits unconditionally because it
    // cannot be an expression). Invalid (the default) ⇒ no guard; the
    // probe commits on structural success exactly as before. Config-
    // sourced — the engine never hardcodes which rule is a "cast".
    RuleId                     typeNameCommitRule{};
    // FC4 c1 (M4): the guard's UNKNOWN-name polarity (see
    // TypeNameCommitPolarity). The bare-string `commitRequiresTypeName`
    // form keeps the FC2 default; the object form
    // `{ "rule": ..., "polarity": "requireKnownType" }` selects the
    // strict C 6.7.6.3p11 behavior. Meaningless unless
    // `typeNameCommitRule.valid()`.
    TypeNameCommitPolarity     typeNameCommitPolarity =
        TypeNameCommitPolarity::PreferType;
};

} // namespace dss::detail

} // namespace dss
