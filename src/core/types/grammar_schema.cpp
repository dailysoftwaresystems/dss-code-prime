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
// constructed cursors before they touch real data.
[[nodiscard]] detail::Position const* lookupPos(
    detail::GrammarSchemaData const& d, SchemaCursor cur) noexcept {
    if (!cur.valid()) return nullptr;
    auto it = d.compiledRules.find(cur.rule().v);
    if (it == d.compiledRules.end()) return nullptr;
    auto const& positions = it->second.positions;
    if (cur.posId() >= positions.size()) return nullptr;
    return &positions[cur.posId()];
}

} // namespace

SchemaCursor GrammarSchema::rootCursor() const noexcept {
    if (!d_.rootRule.valid()) return SchemaCursor{};
    auto it = d_.compiledRules.find(d_.rootRule.v);
    if (it == d_.compiledRules.end()) return SchemaCursor{};
    return SchemaCursor{d_.rootRule, it->second.entryPos};
}

SchemaCursor GrammarSchema::enterRule(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return SchemaCursor{};
    return SchemaCursor{rule, it->second.entryPos};
}

SchemaCursor GrammarSchema::leaveRule(SchemaCursor parentCur) const noexcept {
    auto const* p = lookupPos(d_, parentCur);
    if (p == nullptr) return SchemaCursor{};
    if (p->slotKind() != SlotKind::RuleLeaf) return SchemaCursor{};
    return SchemaCursor{parentCur.rule(), p->nextPos()};
}

SchemaCursor GrammarSchema::routeToRuleLeaf(SchemaCursor parentCur,
                                            RuleId rule) const noexcept {
    auto const* p = lookupPos(d_, parentCur);
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

std::vector<RuleId> GrammarSchema::altRuleBranches(SchemaCursor cur) const noexcept {
    std::vector<RuleId> out;
    if (!cur.valid()) return out;
    auto it = d_.compiledRules.find(cur.rule().v);
    if (it == d_.compiledRules.end()) return out;
    auto const& positions = it->second.positions;

    // Depth-first over AltChoice branch edges, in stored (= declared
    // JSON-array) order — the exact traversal `routeToRuleLeaf` uses,
    // so a rule appears here iff `routeToRuleLeaf(cur, rule)` would
    // find it. `visitedPos` guards positional cycles (a `repeat`'s
    // tie-the-knot loop entry can be reachable from its own branch
    // subtree when the repeated body is nullable); `routeToRuleLeaf`
    // never revisits in loadable grammars, and pruning a revisit
    // cannot drop a rule — every reachable RuleLeaf is collected on
    // the first visit of its position. Counts are tiny (branch fan-
    // out per alt), so linear membership scans beat set machinery.
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
    walk(walk, cur.posId());
    return out;
}

SchemaCursor GrammarSchema::advance(SchemaCursor cur, SchemaTokenId tok) const noexcept {
    auto it = d_.compiledRules.find(cur.rule().v);
    if (it == d_.compiledRules.end()) return SchemaCursor{};
    auto const& positions = it->second.positions;
    if (cur.posId() >= positions.size()) return SchemaCursor{};

    // Walk through AltChoice positions until we hit a TokenLeaf or fail.
    // Each AltChoice routes into the first branch whose precomputed
    // expectedSet contains `tok`. The loader rejects ambiguous alts at
    // load time (C_AmbiguousAlternatives), so the "first match wins"
    // behaviour is unambiguous in any schema that loaded successfully.
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
                for (auto const& t : positions[bid].expectedSet()) {
                    if (t.v == tok.v) { matched = bid; found = true; break; }
                }
                if (found) break;
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
    auto const* p = lookupPos(d_, cur);
    if (p == nullptr) return {};
    return p->expectedSet();
}

SlotKind GrammarSchema::slotKind(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    return p == nullptr ? SlotKind::End : p->slotKind();
}

RuleId GrammarSchema::slotRuleRef(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    if (p == nullptr || p->slotKind() != SlotKind::RuleLeaf) return InvalidRule;
    return p->ruleId();
}

bool GrammarSchema::isAtEndOfRule(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    return p != nullptr && p->slotKind() == SlotKind::End;
}

bool GrammarSchema::isSpeculativeAlt(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    return p != nullptr && p->slotKind() == SlotKind::AltChoice && p->speculative();
}

std::uint16_t GrammarSchema::lookahead(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    return (p != nullptr && p->slotKind() == SlotKind::AltChoice) ? p->lookahead() : 0;
}

bool GrammarSchema::nullableTail(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    return p != nullptr && p->nullableTail();
}

SchemaCursor GrammarSchema::nullableBranch(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    if (p == nullptr || p->slotKind() != SlotKind::AltChoice) return SchemaCursor{};
    auto it = d_.compiledRules.find(cur.rule().v);
    if (it == d_.compiledRules.end()) return SchemaCursor{};
    auto const& positions = it->second.positions;
    for (auto bid : p->branches()) {
        if (bid >= positions.size()) continue;
        if (positions[bid].nullableTail()) {
            return SchemaCursor{cur.rule(), bid};
        }
    }
    return SchemaCursor{};
}

std::span<SchemaTokenId const> GrammarSchema::firstSetOf(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return {};
    return it->second.firstSet;
}

std::span<SchemaTokenId const> GrammarSchema::followSetOf(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return {};
    return it->second.followSet;
}

std::size_t GrammarSchema::predictivePrefixLen(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return 0;
    return it->second.predictivePrefix.size();
}

std::span<SchemaTokenId const>
GrammarSchema::predictivePrefixAt(RuleId rule, std::size_t offset) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return {};
    auto const& pfx = it->second.predictivePrefix;
    if (offset >= pfx.size()) return {};
    return pfx[offset];
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
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return false;
    return it->second.nullable;
}

bool GrammarSchema::isExprRule(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return false;
    return it->second.isExpr;
}

bool GrammarSchema::isAutoInternedWrapperRule(RuleId rule) const noexcept {
    return d_.wrapperRuleIds.contains(rule.v);
}

RuleId GrammarSchema::exprAtom(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return RuleId{};
    return it->second.exprAtom;
}

std::int32_t GrammarSchema::exprMinPrecedence(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return 0;
    return it->second.exprMinPrecedence;
}

RuleId GrammarSchema::typeNameCommitRule(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return RuleId{};
    return it->second.typeNameCommitRule;
}

TypeNameCommitPolarity
GrammarSchema::typeNameCommitPolarity(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return TypeNameCommitPolarity::PreferType;
    return it->second.typeNameCommitPolarity;
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
    auto const* p = lookupPos(d_, cur);
    return p != nullptr && p->nullableTail();
}

} // namespace dss
