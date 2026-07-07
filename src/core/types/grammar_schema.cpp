#include "core/types/grammar_schema.hpp"

#include "core/types/config_path_walk.hpp"
#include "core/types/grammar_schema_json.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace dss {

namespace {

// c97 sealing helper: the depth-first RuleLeaf-branch enumeration over one
// rule's position graph — the SAME walk (same order, same first-occurrence
// dedup, same cycle guard) the former per-call `altRuleBranches` ran; now
// executed ONCE per AltChoice position at schema construction and stored on
// the Position. Pure config-data transform; names no token/rule/language.
[[nodiscard]] std::vector<RuleId> collectAltBranchRules(
    std::vector<detail::Position> const& positions, std::uint32_t startPos) {
    std::vector<RuleId> out;
    std::vector<std::uint32_t> visitedPos;
    auto walk = [&](auto&& self, std::uint32_t posId) -> void {
        if (posId >= positions.size()) return;
        for (auto const seen : visitedPos) {
            if (seen == posId) return;
        }
        visitedPos.push_back(posId);
        auto const& p = positions[posId];
        if (p.slotKind() == SlotKind::RuleLeaf) {
            const RuleId r = p.ruleId();
            for (auto const existing : out) {
                if (existing.v == r.v) return;   // first occurrence wins
            }
            out.push_back(r);
            return;
        }
        if (p.slotKind() == SlotKind::AltChoice) {
            for (auto bid : p.branches()) self(self, bid);
        }
        // TokenLeaf / End: not a rule branch — token routing goes
        // through `advance`.
    };
    walk(walk, startPos);
    return out;
}

} // namespace

GrammarSchema::GrammarSchema(detail::GrammarSchemaData&& d) noexcept : d_(std::move(d)) {
#ifndef NDEBUG
    // Cross-check the loader-computed `maxLexemeLength` against a fresh
    // recomputation. Catches a loader-side bug that would otherwise
    // silently truncate the tokenizer's longest-match window. Debug-
    // only — the production builds trust the loader (single call site
    // for the field).
    std::size_t recomputed = 0;
    for (auto const& [lex, meanings] : d_.lexemeTable) {
        (void)meanings;
        if (lex.size() > recomputed) recomputed = lex.size();
    }
    for (auto const& [modeId, table] : d_.lexerModeTokens) {
        (void)modeId;
        for (auto const& [lex, meanings] : table) {
            (void)meanings;
            if (lex.size() > recomputed) recomputed = lex.size();
        }
    }
    assert(recomputed == d_.maxLexemeLength
           && "GrammarSchemaData::maxLexemeLength out of sync with lexemeTable");
#endif

    // ── c97 sealing pass ──────────────────────────────────────────────
    // Drain the loader's build-time `compiledRules` map into the dense
    // RuleId-indexed vector, then derive the O(1) query companions —
    // per-set membership bitsets + the precomputed AltChoice branch
    // enumerations. Load-time-only work (one pass over static config
    // data); every per-token query downstream becomes an array index /
    // bit test. RuleIds are dense interner ids (1..N), so the vector is
    // sized by the interner (belt-and-braces: also covers any key above
    // it). Runs for EVERY construction path — the JSON loader and tests
    // that build GrammarSchemaData directly.
    {
        std::size_t denseSize = d_.rules ? d_.rules->size() : 0;
        for (auto const& [rid, rule] : d_.compiledRules) {
            (void)rule;
            if (rid + 1 > denseSize) denseSize = rid + 1;
        }
        compiledDense_.resize(denseSize);
        for (auto& [rid, rule] : d_.compiledRules) {
            compiledDense_[rid] = std::move(rule);
        }
        d_.compiledRules.clear();

        std::size_t const universe =
            d_.schemaTokens ? d_.schemaTokens->size() : 0;
        for (auto& rule : compiledDense_) {
            rule.firstBits = detail::buildTokenBits(rule.firstSet, universe);
            rule.prefixBits.clear();
            rule.prefixBits.reserve(rule.predictivePrefix.size());
            for (auto const& offsetSet : rule.predictivePrefix) {
                rule.prefixBits.push_back(
                    detail::buildTokenBits(offsetSet, universe));
            }
            for (auto& pos : rule.positions) {
                pos.sealExpectedBits(universe);
                // Seal the branch enumeration for EVERY slot kind — the
                // walk itself resolves each correctly (AltChoice → DFS,
                // RuleLeaf → {rule}, TokenLeaf/End → empty), preserving
                // the former per-call function's full contract.
                pos.sealAltBranchRules(collectAltBranchRules(
                    rule.positions,
                    static_cast<std::uint32_t>(&pos - rule.positions.data())));
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Loaders — thin shims over the JSON-aware loader.
// ─────────────────────────────────────────────────────────────────────────

LoadResult<std::shared_ptr<GrammarSchema>> GrammarSchema::loadFromFile(
    std::filesystem::path const& path) {

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_MissingField, DiagnosticSeverity::Error,
             path.string(),
             "cannot open file"}});
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return loadFromText(std::move(buf).str(), path.string());
}

LoadResult<std::shared_ptr<GrammarSchema>> GrammarSchema::loadShipped(std::string_view name) {
    auto path = findShippedConfig({name, "sources", ".lang.json", "language",
                                   DiagnosticCode::C_InvalidLanguageName});
    if (!path) return std::unexpected(std::move(path).error());
    return loadFromFile(*path);
}

LoadResult<std::shared_ptr<GrammarSchema>> GrammarSchema::loadFromText(
    std::string_view jsonText,
    std::string_view sourceLabel) {

    return detail::buildSchemaFromJsonText(jsonText, sourceLabel);
}

// ─────────────────────────────────────────────────────────────────────────
// Read-only queries
// ─────────────────────────────────────────────────────────────────────────

std::span<LexemeMeaning const> GrammarSchema::lookupLexeme(std::string_view lexeme) const noexcept {
    auto it = d_.lexemeTable.find(std::string{lexeme});
    if (it == d_.lexemeTable.end()) return {};
    return it->second;
}

bool GrammarSchema::isEmptySpace(SchemaTokenId id) const noexcept {
    return d_.emptySpaceTokens.contains(id.v);
}

NodeFlags GrammarSchema::flagsForKind(SchemaTokenId id) const noexcept {
    // No schema field populates per-kind flags today, so every kind
    // returns None. The accessor is the structural channel the
    // numeric-literal emit site reads — a future `literalFlags`
    // schema field plugs in here without a tokenizer change. The
    // valid-id check bakes the contract NOW, while there are zero
    // data-bearing callers: future schemas wiring real data in must
    // not silently accept an invalid id.
    if (!id.valid()) return NodeFlags::None;
    return NodeFlags::None;
}

std::span<LexerMode const> GrammarSchema::lexerModes() const noexcept {
    // Slot 0 is the InvalidLexerMode sentinel — internal indexing
    // detail. Real modes start at index 1; subspan(1) hides the
    // sentinel from every iterating consumer.
    if (d_.lexerModes.size() <= 1) return {};
    return std::span<LexerMode const>(d_.lexerModes).subspan(1);
}

LexerModeId GrammarSchema::findLexerMode(std::string_view name) const noexcept {
    auto it = d_.lexerModeIds.find(std::string{name});
    return (it == d_.lexerModeIds.end()) ? InvalidLexerMode : it->second;
}

LexerMode const& GrammarSchema::lexerMode(LexerModeId id) const noexcept {
    if (!id.valid() || id.v >= d_.lexerModes.size()) {
        std::fprintf(stderr,
            "dss::GrammarSchema::lexerMode: invalid LexerModeId (v=%u, "
            "table_size=%zu)\n",
            id.v, d_.lexerModes.size());
        std::abort();
    }
    return d_.lexerModes[id.v];
}

StringStyle const* GrammarSchema::stringStyle(LexemeMeaning const& m) const noexcept {
    if (m.schemaId.v != d_.id.v) {
        std::fprintf(stderr,
            "dss::GrammarSchema::stringStyle: LexemeMeaning belongs to a "
            "different schema (meaning.schemaId=%u, this.schemaId=%u)\n",
            m.schemaId.v, d_.id.v);
        std::abort();
    }
    if (!m.stringStyleId.valid()) return nullptr;
    if (m.stringStyleId.v >= d_.stringStyles.size()) {
        std::fprintf(stderr,
            "dss::GrammarSchema::stringStyle: out-of-range stringStyleId "
            "(v=%u, pool_size=%zu)\n",
            m.stringStyleId.v, d_.stringStyles.size());
        std::abort();
    }
    return &d_.stringStyles[m.stringStyleId.v];
}

std::span<LexemeMeaning const>
GrammarSchema::lookupLexemeInMode(LexerModeId mode, std::string_view lexeme) const noexcept {
    if (!mode.valid() || mode.v >= d_.lexerModes.size()) {
        std::fprintf(stderr,
            "dss::GrammarSchema::lookupLexemeInMode: invalid LexerModeId "
            "(v=%u, table_size=%zu)\n",
            mode.v, d_.lexerModes.size());
        std::abort();
    }
    auto modeIt = d_.lexerModeTokens.find(mode.v);
    if (modeIt == d_.lexerModeTokens.end()) return {};
    auto lexIt = modeIt->second.find(std::string{lexeme});
    if (lexIt == modeIt->second.end()) return {};
    return lexIt->second;
}

namespace {

// Locate the position record for a cursor, or nullptr if the cursor is
// invalid / out of range. The cursor's `posId == 0` sentinel matches the
// loader's `positions[0]` placeholder, so this also blocks default-
// constructed cursors before they touch real data. (c97: reads the dense
// table — an id with no compiled body has an EMPTY positions vector, so
// every posId is out of range, exactly the former map-miss result.)
[[nodiscard]] detail::Position const* lookupPos(
    detail::CompiledRule const* rule, SchemaCursor cur) noexcept {
    if (!cur.valid() || rule == nullptr) return nullptr;
    auto const& positions = rule->positions;
    if (cur.posId() >= positions.size()) return nullptr;
    return &positions[cur.posId()];
}

} // namespace

SchemaCursor GrammarSchema::rootCursor() const noexcept {
    if (!d_.rootRule.valid()) return SchemaCursor{};
    auto const* r = ruleRow(d_.rootRule.v);
    // entryPos == 0 ⇒ no compiled body (see CompiledRule) — the former
    // map-miss result. A real rule's entry is never the 0 sentinel.
    if (r == nullptr || r->entryPos == 0) return SchemaCursor{};
    return SchemaCursor{d_.rootRule, r->entryPos};
}

SchemaCursor GrammarSchema::enterRule(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr || r->entryPos == 0) return SchemaCursor{};
    return SchemaCursor{rule, r->entryPos};
}

SchemaCursor GrammarSchema::leaveRule(SchemaCursor parentCur) const noexcept {
    auto const* p = lookupPos(ruleRow(parentCur.rule().v), parentCur);
    if (p == nullptr) return SchemaCursor{};
    if (p->slotKind() != SlotKind::RuleLeaf) return SchemaCursor{};
    return SchemaCursor{parentCur.rule(), p->nextPos()};
}

SchemaCursor GrammarSchema::routeToRuleLeaf(SchemaCursor parentCur,
                                            RuleId rule) const noexcept {
    auto const* p = lookupPos(ruleRow(parentCur.rule().v), parentCur);
    if (p == nullptr) return SchemaCursor{};
    if (p->slotKind() == SlotKind::RuleLeaf && p->ruleId().v == rule.v) {
        return parentCur;
    }
    if (p->slotKind() == SlotKind::AltChoice) {
        // Try each branch; first match wins (the loader already rejects
        // ambiguous alts at load time via C_AmbiguousAlternatives, so
        // any two RuleLeaf branches in the same AltChoice would have
        // been flagged before reaching here).
        for (auto bid : p->branches()) {
            SchemaCursor probe{parentCur.rule(), bid};
            auto found = routeToRuleLeaf(probe, rule);
            if (found.valid()) return found;
        }
    }
    return SchemaCursor{};
}

std::span<RuleId const> GrammarSchema::altRuleBranches(SchemaCursor cur) const noexcept {
    // c97: the DFS over AltChoice branch edges (declared JSON-array order,
    // first occurrence wins, positional-cycle guard — see
    // `collectAltBranchRules`) is precomputed per position at schema
    // construction; this is now a span read. Same contract as the former
    // per-call walk, including `{rule}` at a RuleLeaf position and empty
    // for invalid cursors / token positions.
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    if (p == nullptr) return {};
    return p->altBranchRules();
}

SchemaCursor GrammarSchema::advance(SchemaCursor cur, SchemaTokenId tok) const noexcept {
    auto const* r = ruleRow(cur.rule().v);
    if (r == nullptr) return SchemaCursor{};
    auto const& positions = r->positions;
    if (cur.posId() >= positions.size()) return SchemaCursor{};

    // Walk through AltChoice positions until we hit a TokenLeaf or fail.
    // Each AltChoice routes into the first branch whose precomputed
    // expectedSet contains `tok` (c97: an O(1) bit test on the sealed
    // bitset — same first-match-wins semantics as the former linear scan).
    // The loader rejects ambiguous alts at load time
    // (C_AmbiguousAlternatives), so the "first match wins" behaviour is
    // unambiguous in any schema that loaded successfully.
    std::uint32_t curPosId = cur.posId();
    while (true) {
        auto const& p = positions[curPosId];
        if (p.slotKind() == SlotKind::TokenLeaf) {
            if (p.tokenId().v == tok.v) {
                return SchemaCursor{cur.rule(), p.nextPos()};
            }
            return SchemaCursor{};
        }
        if (p.slotKind() == SlotKind::AltChoice) {
            std::uint32_t matched = 0;
            bool found = false;
            for (auto bid : p.branches()) {
                if (detail::tokenBitsContain(positions[bid].expectedBits(),
                                             tok.v)) {
                    matched = bid;
                    found   = true;
                    break;
                }
            }
            if (!found) return SchemaCursor{};
            curPosId = matched;
            continue;
        }
        // RuleLeaf or End — not advanceable by a token. Caller must
        // descend via enterRule (RuleLeaf) or pop back to the parent
        // (End).
        return SchemaCursor{};
    }
}

std::span<SchemaTokenId const> GrammarSchema::expectedSet(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    if (p == nullptr) return {};
    return p->expectedSet();
}

bool GrammarSchema::expectedSetContains(SchemaCursor cur,
                                        SchemaTokenId tok) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    if (p == nullptr) return false;
    return detail::tokenBitsContain(p->expectedBits(), tok.v);
}

bool GrammarSchema::firstSetContains(RuleId rule,
                                     SchemaTokenId tok) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return false;
    return detail::tokenBitsContain(r->firstBits, tok.v);
}

bool GrammarSchema::predictivePrefixExcludes(RuleId rule, std::size_t offset,
                                             SchemaTokenId tok) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return false;                    // no rule → no constraint
    if (offset >= r->prefixBits.size()) return false;  // undefined → no constraint
    auto const& bits = r->prefixBits[offset];
    if (bits.empty()) return false;                    // empty set → no constraint
    return !detail::tokenBitsContain(bits, tok.v);
}

SlotKind GrammarSchema::slotKind(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return p == nullptr ? SlotKind::End : p->slotKind();
}

RuleId GrammarSchema::slotRuleRef(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    if (p == nullptr || p->slotKind() != SlotKind::RuleLeaf) return InvalidRule;
    return p->ruleId();
}

bool GrammarSchema::isAtEndOfRule(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return p != nullptr && p->slotKind() == SlotKind::End;
}

bool GrammarSchema::isSpeculativeAlt(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return p != nullptr && p->slotKind() == SlotKind::AltChoice && p->speculative();
}

std::uint16_t GrammarSchema::lookahead(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return (p != nullptr && p->slotKind() == SlotKind::AltChoice) ? p->lookahead() : 0;
}

bool GrammarSchema::nullableTail(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return p != nullptr && p->nullableTail();
}

SchemaCursor GrammarSchema::nullableBranch(SchemaCursor cur) const noexcept {
    auto const* r = ruleRow(cur.rule().v);
    auto const* p = lookupPos(r, cur);
    if (p == nullptr || p->slotKind() != SlotKind::AltChoice) return SchemaCursor{};
    auto const& positions = r->positions;
    for (auto bid : p->branches()) {
        if (bid >= positions.size()) continue;
        if (positions[bid].nullableTail()) {
            return SchemaCursor{cur.rule(), bid};
        }
    }
    return SchemaCursor{};
}

std::span<SchemaTokenId const> GrammarSchema::firstSetOf(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return {};
    return r->firstSet;
}

std::span<SchemaTokenId const> GrammarSchema::followSetOf(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return {};
    return r->followSet;
}

std::size_t GrammarSchema::predictivePrefixLen(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return 0;
    return r->predictivePrefix.size();
}

std::span<SchemaTokenId const>
GrammarSchema::predictivePrefixAt(RuleId rule, std::size_t offset) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return {};
    auto const& pfx = r->predictivePrefix;
    if (offset >= pfx.size()) return {};
    return pfx[offset];
}

bool GrammarSchema::isContextualKind(SchemaTokenId kind) const noexcept {
    // D-PARSE-PREDICTIVE-PRUNE-CONTEXTUAL-KEYWORD. O(1); the set is EMPTY for
    // every grammar with no contextual keyword (Strict policy + no per-keyword
    // `contextual: true`) — so the prune's deep-nest O(N) win is unaffected for
    // the non-contextual case (every shipped c-subset speculative alt).
    return kind.valid() && d_.contextualKinds.contains(kind.v);
}

std::span<SchemaTokenId const> GrammarSchema::syncTokens() const noexcept {
    return d_.syncTokens;
}

std::span<TypeExtensionDescriptor const> GrammarSchema::typeExtensions() const noexcept {
    return d_.typeExtensions;
}

std::span<std::string const> GrammarSchema::artifactProfiles() const noexcept {
    return d_.artifactProfiles;
}

ImportConfig const& GrammarSchema::imports() const noexcept {
    return d_.imports;
}

PreprocessConfig const& GrammarSchema::preprocess() const noexcept {
    return d_.preprocess;
}

bool GrammarSchema::isNullable(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    return r != nullptr && r->nullable;
}

bool GrammarSchema::isExprRule(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    return r != nullptr && r->isExpr;
}

bool GrammarSchema::isAutoInternedWrapperRule(RuleId rule) const noexcept {
    return d_.wrapperRuleIds.contains(rule.v);
}

RuleId GrammarSchema::exprAtom(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return RuleId{};
    return r->exprAtom;
}

std::int32_t GrammarSchema::exprMinPrecedence(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return 0;
    return r->exprMinPrecedence;
}

RuleId GrammarSchema::typeNameCommitRule(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return RuleId{};
    return r->typeNameCommitRule;
}

TypeNameCommitPolarity
GrammarSchema::typeNameCommitPolarity(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return TypeNameCommitPolarity::PreferType;
    return r->typeNameCommitPolarity;
}

bool GrammarSchema::commitAfterPrefix(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    return r != nullptr && r->commitAfterPrefix;
}

ExprWrapperRules GrammarSchema::exprWrapperRules(RuleId rule) const noexcept {
    auto it = d_.exprWrapperRules.find(rule.v);
    if (it == d_.exprWrapperRules.end()) return ExprWrapperRules{};
    return it->second;
}

NumberStyle const* GrammarSchema::numberStyle() const noexcept {
    return d_.numberStyle.has_value() ? &(*d_.numberStyle) : nullptr;
}

SemanticConfig const& GrammarSchema::semantics() const noexcept {
    return d_.semantics;
}

HirLoweringConfig const& GrammarSchema::hirLowering() const noexcept {
    return d_.hirLowering;
}

bool GrammarSchema::isTokenValidInScope(SchemaTokenId tok,
                                       std::span<ScopeKind const> stack) const noexcept {
    // Walk the scope stack top-down so the innermost scope's rules win —
    // a `forbid` listed on the innermost frame applies even if an outer
    // frame allows the token.
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        auto scopeIt = d_.scopeForbid.find(static_cast<std::uint16_t>(*it));
        if (scopeIt != d_.scopeForbid.end() && scopeIt->second.contains(tok.v)) {
            return false;
        }
    }
    return true;
}

bool GrammarSchema::canEndSource(SchemaCursor cur) const noexcept {
    if (cur.rule().v != d_.rootRule.v) return false;
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return p != nullptr && p->nullableTail();
}

} // namespace dss
