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

SchemaCursor GrammarSchema::rootCursor() const noexcept {
    if (!d_.rootRule.valid()) return SchemaCursor{};
    return SchemaCursor{d_.rootRule.v, /*position*/ 0, /*parent*/ 0, /*alt*/ 0};
}

std::span<std::string const> GrammarSchema::expectedAt(SchemaCursor cur) const noexcept {
    if (!cur.valid()) return {};
    auto it = d_.expectedAt.find(cur.shapeId());
    if (it == d_.expectedAt.end()) return {};
    return it->second;
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
    // First-iteration policy: source can end at root (top-level scope)
    // with the cursor still on the root rule. Per-shape canEndSource flags
    // declared in config will be honoured here once T5 wires the shape
    // graph through advance().
    return cur.valid() && cur.shapeId() == d_.rootRule.v && cur.position() == 0;
}

} // namespace dss
