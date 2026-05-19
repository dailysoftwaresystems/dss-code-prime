#include "core/types/grammar_schema.hpp"

#include "core/types/grammar_schema_json.hpp"

#include <fstream>
#include <sstream>
#include <utility>

namespace dss {

GrammarSchema::GrammarSchema(detail::GrammarSchemaData&& d) noexcept : d_(std::move(d)) {}

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
    // Reject obviously-bad names (path-separators, dotfiles, empties) up
    // front. The loader is only meant to resolve `csharp` / `dart` /
    // `tsql` / `sqlite` / `toy` — never arbitrary paths.
    if (name.empty() || name.find('/') != std::string_view::npos
        || name.find('\\') != std::string_view::npos
        || name.front() == '.') {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_InvalidLanguageName, DiagnosticSeverity::Error,
             std::string{name}, "invalid shipped-language name"}});
    }

    namespace fs = std::filesystem;
    const std::string leaf = std::string{name} + ".lang.json";

    // Walk up the directory tree from cwd looking for
    // `src/source-config/languages/<name>.lang.json`. This works whether
    // the binary is invoked from the repo root, from build/, or from a
    // nested tests/core build subdirectory — ctest's cwd varies.
    std::error_code ec;
    fs::path here = fs::current_path(ec);
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        const fs::path candidate = here / "src" / "source-config" / "languages" / leaf;
        if (fs::exists(candidate, ec)) {
            return loadFromFile(candidate);
        }
        const fs::path parent = here.parent_path();
        if (parent == here) break;     // hit the root
        here = parent;
    }

    return std::unexpected(std::vector<ConfigDiagnostic>{
        {DiagnosticCode::C_InvalidLanguageName, DiagnosticSeverity::Error,
         std::string{name},
         "no shipped language config found in src/source-config/languages/"}});
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

namespace {

// Helper: locate the position record for a cursor, or nullptr if the
// cursor is invalid / out of range. Const access; the cursor methods are
// read-only.
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
    if (p->slotKind != detail::SlotKind::RuleLeaf) return SchemaCursor{};
    return SchemaCursor{parentCur.rule(), p->nextPos};
}

SchemaCursor GrammarSchema::advance(SchemaCursor cur, SchemaTokenId tok) const noexcept {
    auto it = d_.compiledRules.find(cur.rule().v);
    if (it == d_.compiledRules.end()) return SchemaCursor{};
    auto const& positions = it->second.positions;
    if (cur.posId() >= positions.size()) return SchemaCursor{};

    // Walk through AltChoice positions until we hit a TokenLeaf or fail.
    // Each AltChoice routes via the branch whose precomputed expectedSet
    // contains `tok`. Loop is bounded by the number of nested alts; in
    // practice depth 2–3.
    std::uint32_t curPosId = cur.posId();
    while (true) {
        auto const& p = positions[curPosId];
        if (p.slotKind == detail::SlotKind::TokenLeaf) {
            if (p.tokenId.v == tok.v) {
                return SchemaCursor{cur.rule(), p.nextPos};
            }
            return SchemaCursor{};
        }
        if (p.slotKind == detail::SlotKind::AltChoice) {
            std::uint32_t matched = 0;
            bool found = false;
            for (auto bid : p.branches) {
                auto const& branch = positions[bid];
                for (auto const& t : branch.expectedSet) {
                    if (t.v == tok.v) {
                        matched = bid;
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            if (!found) return SchemaCursor{};
            curPosId = matched;
            continue;
        }
        // RuleLeaf or End — can't be advanced by a token. The caller
        // must descend via enterRule or return to the parent first.
        return SchemaCursor{};
    }
}

std::span<SchemaTokenId const> GrammarSchema::expectedSet(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    if (p == nullptr) return {};
    return p->expectedSet;
}

detail::SlotKind GrammarSchema::slotKind(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    if (p == nullptr) return detail::SlotKind::End;
    return p->slotKind;
}

RuleId GrammarSchema::slotRuleRef(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    if (p == nullptr || p->slotKind != detail::SlotKind::RuleLeaf) return InvalidRule;
    return p->ruleId;
}

bool GrammarSchema::isAtEndOfRule(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(d_, cur);
    return p != nullptr && p->slotKind == detail::SlotKind::End;
}

std::span<SchemaTokenId const> GrammarSchema::firstSetOf(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return {};
    return it->second.firstSet;
}

bool GrammarSchema::isNullable(RuleId rule) const noexcept {
    auto it = d_.compiledRules.find(rule.v);
    if (it == d_.compiledRules.end()) return false;
    return it->second.nullable;
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
    return p != nullptr && p->nullableTail;
}

} // namespace dss
