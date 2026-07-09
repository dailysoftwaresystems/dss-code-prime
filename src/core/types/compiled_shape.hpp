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

// c97 (compile-time-performance arc): O(1) token-set membership.
//
// The parser's hot loop asks "is token kind K in this set?" several times
// per token (expectedSet / FIRST / predictive-prefix gates). Token kinds are
// DENSE interned integers (SchemaTokenInterner ids, 1..N), so each set gets
// a companion BITSET — `bits[v>>6] >> (v&63) & 1` — built ONCE at schema
// construction (`GrammarSchema`'s ctor sealing pass) from the vector form.
// The vector form REMAINS the source of truth and the iteration surface
// (diagnostic rendering walks it); the bits are a load-time derived index.
// Pure config-data transform — no token, rule, or language is named.
//
// An EMPTY bits span contains nothing — callers that need "empty set means
// no constraint" semantics must test emptiness explicitly (as the
// predictive-prefix prune does).
[[nodiscard]] inline bool tokenBitsContain(std::span<std::uint64_t const> bits,
                                           std::uint32_t v) noexcept {
    std::size_t const word = static_cast<std::size_t>(v) >> 6;
    return word < bits.size()
        && ((bits[word] >> (v & 63u)) & 1u) != 0u;
}

// Build the bitset companion for one token-id set. `universe` is the
// schema's token-id count (interner size); ids ≥ universe (corrupt) are
// still admitted by growing the block count — membership stays exact.
[[nodiscard]] inline std::vector<std::uint64_t>
buildTokenBits(std::span<SchemaTokenId const> set, std::size_t universe) {
    std::vector<std::uint64_t> bits;
    if (set.empty()) return bits;
    bits.resize((universe >> 6) + 1, 0u);
    for (auto const t : set) {
        std::size_t const word = static_cast<std::size_t>(t.v) >> 6;
        if (word >= bits.size()) bits.resize(word + 1, 0u);
        bits[word] |= (std::uint64_t{1} << (t.v & 63u));
    }
    return bits;
}

// One position in a rule's compiled shape body. Built once by the loader
// via the named factories below; cursor reads through the const accessors.
// `tokenId`, `ruleId`, `nextPos`, and `branches` are slot-kind-dependent:
// they're only meaningful for the slot kind the factory targeted, and the
// factory enforces that pairing.
//
// `nullableTail` is the only field updated post-construction — the loader's
// fixed-point pass for canEndSource semantics flips it false→true. (c97:
// plus the GrammarSchema ctor's sealing pass, which derives `expectedBits`
// and `altBranchRules` from the final loader-built fields.)
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

    // c97: bitset companion of `expectedSet()` (O(1) membership; built by
    // the GrammarSchema ctor's sealing pass AFTER every loader fixed-point
    // finished mutating `expectedSet_`). Empty until sealed.
    [[nodiscard]] std::span<std::uint64_t const> expectedBits() const noexcept {
        return expectedBits_;
    }

    // c97: the precomputed depth-first RuleLeaf-branch enumeration for an
    // AltChoice position — the EXACT result the former per-call
    // `GrammarSchema::altRuleBranches` DFS produced (declared JSON-array
    // order, first occurrence wins), computed ONCE at schema sealing so the
    // per-speculative-token query is a span read instead of a fresh DFS +
    // two vector allocations. Empty for non-AltChoice positions.
    [[nodiscard]] std::span<RuleId const> altBranchRules() const noexcept {
        return altBranchRules_;
    }

    // D-PARSE-SPECULATIVE-OPTIONAL: for an AltChoice built from the
    // `{"optional": X}` sugar, `hasSkipBranch()` is true and `skipBranch()`
    // is the position-id of the SECOND branch — the skip/continuation
    // (`cont`) the optional falls through to when X is absent. It is NOT one
    // of the optional's own alternatives, so a SPECULATIVE optional excludes
    // it from candidate enumeration (see `collectAltBranchRules`). Unset
    // (false / 0) for `"alt"`/`"repeat"`-built AltChoices and every leaf.
    [[nodiscard]] bool          hasSkipBranch() const noexcept { return hasSkipBranch_; }
    [[nodiscard]] std::uint32_t skipBranch()    const noexcept { return skipBranch_; }

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
    // D-PARSE-SPECULATIVE-OPTIONAL: record the optional sugar's skip/
    // continuation branch (`cont`, the second branch). Set by the
    // `"optional"` builder in `PositionBuilder::build`.
    void setSkipBranch(std::uint32_t pos) noexcept {
        skipBranch_    = pos;
        hasSkipBranch_ = true;
    }

    // c97 sealing-pass writers (GrammarSchema ctor only): derive the O(1)
    // membership bits / the precomputed alt-branch list from the FINAL
    // loader-built fields. Not for loader use — the loader's fixed-points
    // still mutate `expectedSet_` after construction.
    void sealExpectedBits(std::size_t universe) {
        expectedBits_ = buildTokenBits(expectedSet_, universe);
    }
    void sealAltBranchRules(std::vector<RuleId> rules) noexcept {
        altBranchRules_ = std::move(rules);
    }

private:
    SlotKind        slotKind_    = SlotKind::End;
    SchemaTokenId   tokenId_;
    RuleId          ruleId_;
    std::uint32_t   nextPos_     = 0;
    std::vector<std::uint32_t> branches_;
    std::vector<SchemaTokenId> expectedSet_;
    std::vector<std::uint64_t> expectedBits_;    // c97: sealed bitset of expectedSet_
    std::vector<RuleId>        altBranchRules_;  // c97: sealed AltChoice DFS result
    std::uint32_t   skipBranch_   = 0;      // D-PARSE-SPECULATIVE-OPTIONAL: cont branch pos-id
    bool            nullableTail_ = false;
    bool            speculative_  = false;
    bool            hasSkipBranch_ = false; // D-PARSE-SPECULATIVE-OPTIONAL: skipBranch_ is set
    std::uint16_t   lookahead_    = 0;
};

struct CompiledRule {
    // Index into `positions` for the rule's entry point. `positions[0]`
    // is reserved as a sentinel so cursor `posId == 0` means "invalid",
    // matching the strong-id zero-is-invalid pattern.
    //
    // c97: `entryPos == 0` doubles as the "no compiled body" marker in the
    // GrammarSchema's dense rule table — a real compiled rule's entry is
    // never the sentinel position, so a default-constructed CompiledRule at
    // an index with no loader entry (e.g. an auto-interned Pratt wrapper
    // rule) reproduces exactly the former unordered_map-miss behavior.
    std::uint32_t              entryPos = 0;
    std::vector<Position>      positions;
    std::vector<SchemaTokenId> firstSet;
    std::vector<SchemaTokenId> followSet;
    // c97: bitset companion of `firstSet` (O(1) membership) + per-offset
    // bitset companions of `predictivePrefix`, both built by the
    // GrammarSchema ctor's sealing pass. Empty until sealed.
    std::vector<std::uint64_t>              firstBits;
    std::vector<std::vector<std::uint64_t>> prefixBits;
    bool                       nullable = false;

    // LL(k) PREDICTIVE PREFIX (speculative-alt candidate pruning).
    //
    // A bounded, per-offset over-approximation of the token sequences that
    // can BEGIN a derivation of this rule. `predictivePrefix[i]` is the
    // (sorted) set of token kinds admissible as the (i)-th consumed token,
    // for the leading run of FIXED-WIDTH (single-token) grammar elements:
    //
    //   - prefix[0] == firstSet (the exact 1-token FIRST, always present
    //     for a non-nullable rule with a concrete leading element).
    //   - prefix[i+1] is added only while element i is a TokenLeaf (it
    //     consumes EXACTLY one token, so grammar-element offset == input
    //     offset stays exact). The run stops — and the prefix ends — at the
    //     first element that is a sub-rule (RuleLeaf: variable width), a
    //     branch/optional/loop (AltChoice: variable width), or End.
    //
    // SOUNDNESS: because every offset before the stop corresponds to a
    // single-token element, `prefix[i]` is the EXACT set of admissible
    // tokens at input offset i; beyond the stop the prefix is simply absent
    // (no constraint). A speculative-alt prune that rejects a candidate only
    // when `peek(i)` ∉ `prefix[i]` for some defined `i` therefore never
    // drops a candidate that could legitimately match — it is a sound
    // over-approximation of the candidate's accepted-prefix set. The prune
    // is purely config-derived (FIRST sets + the position table) and names
    // no token, language, or rule.
    //
    // Empty (size 0) when the rule's entry is itself an AltChoice or the
    // rule is nullable/empty: such rules carry no fixed leading prefix, so
    // only the standard 1-token FIRST gate applies (the pre-existing
    // behavior). The loader fills this in `computePredictivePrefixes`.
    std::vector<std::vector<SchemaTokenId>> predictivePrefix;

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

    // commitAfterPrefix CUT (PEG "cut"; D-CSUBSET-LABEL-BUDGET-CLIFF, p19
    // Cluster G c31). When true, a speculative probe of this rule COMMITS
    // as soon as the rule's FIXED leading token-prefix (predictivePrefix,
    // `predictivePrefixLen` tokens) has been consumed without failure — the
    // rest of the rule then parses NON-speculatively (no probe budget),
    // driven by the outer dispatch loop on the still-open frame. The cut is
    // sound only where, after the fixed prefix, no OTHER alternative of the
    // enclosing alt can match (so committing discards nothing); the config
    // author asserts that by setting the flag. Sibling facet to
    // `typeNameCommitRule` — a rule uses at most one. Config-sourced
    // (`commitAfterPrefix` on the shape body); the engine names no token,
    // rule, or language. Default false ⇒ standard rollback-on-failure
    // speculation.
    bool                       commitAfterPrefix = false;
};

} // namespace dss::detail

} // namespace dss
