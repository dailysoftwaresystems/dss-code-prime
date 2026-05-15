#pragma once

#include "core/export.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/schema_cursor.hpp"
#include "core/types/schema_token_interner.hpp"
#include "core/types/scope_kind.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree_node.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward-declares nothing JSON-related — nlohmann/json never enters this
// header. The JSON-aware loader lives in grammar_schema_json.{hpp,cpp};
// public callers go through loadFromFile / loadShipped / loadFromText.

namespace dss {

// Loader-time diagnostic. Distinct from ParseDiagnostic because the
// originating context is a JSON path, not a source span — a malformed
// config doesn't have line/column into the user's source code.
struct DSS_EXPORT ConfigDiagnostic {
    DiagnosticCode      code     = DiagnosticCode::C_MissingField;
    DiagnosticSeverity  severity = DiagnosticSeverity::Error;
    std::string         path;     // JSON pointer ("/shapes/ifStmt/sequence/2") or file path
    std::string         message;
};

// One resolved meaning of a lexeme, sourced from a single entry under
// the config's `tokens` map. A lexeme may have several meanings — the
// builder filters by scope/position, then breaks ties on `priority`
// (lower wins).
struct DSS_EXPORT LexemeMeaning {
    SchemaTokenId    id;
    std::int32_t     priority      = 0;
    NodeFlags        flagsApplied  = NodeFlags::None;
    ScopeKind        opensScope    = ScopeKind::None;
    bool             closesScope   = false;
    // Empty == "valid in every scope"; non-empty == restrict to listed.
    std::span<ScopeKind const> validScopes;
};

// Standard C++23 fallible result. Error channel is the full list of
// diagnostics collected before bailing — the loader keeps walking to
// surface as many problems as possible per run.
template <typename T>
using LoadResult = std::expected<T, std::vector<ConfigDiagnostic>>;

namespace detail {

// Movable POD the JSON loader hands to the GrammarSchema constructor.
// Mirrors the Tree/TreeData split: keeps the schema's read API stable
// while the loader has free reign over field-by-field assembly.
struct DSS_EXPORT GrammarSchemaData {
    std::string                                       name;
    std::string                                       version;
    std::uint32_t                                     schemaVersion = 0;
    std::vector<std::string>                          fileExtensions;
    std::shared_ptr<RuleInterner>                     rules;
    std::shared_ptr<SchemaTokenInterner>              schemaTokens;

    // lexeme → declared meanings, in priority-ascending order (stable —
    // declaration order wins on ties).
    std::unordered_map<std::string, std::vector<LexemeMeaning>> lexemeTable;

    // Backing storage for LexemeMeaning::validScopes spans. Reserved up
    // front by the loader so no reallocation occurs — the spans inside
    // LexemeMeaning point into stable storage.
    std::vector<std::vector<ScopeKind>>               validScopesPool;

    // O(1) "is this token EmptySpace?" without scanning lexemeTable.
    std::unordered_set<std::uint32_t>                 emptySpaceTokens;

    // Per-scope forbidden-token sets — keyed by ScopeKind's underlying
    // value, value = set of SchemaTokenId values.
    std::unordered_map<std::uint16_t, std::unordered_set<std::uint32_t>> scopeForbid;

    // Root rule's id (the "root" shape from config) — anchors rootCursor().
    RuleId rootRule = InvalidRule;

    // Per-rule "expected names" — populated at load time so expectedAt()
    // returns a stable span (no allocation per call).
    std::unordered_map<std::uint32_t, std::vector<std::string>> expectedAt;
};

} // namespace detail

class DSS_EXPORT GrammarSchema {
public:
    // Constructor — the loader is the only caller. Tests can build a
    // GrammarSchemaData directly and construct via this ctor if they need
    // to bypass JSON parsing.
    explicit GrammarSchema(detail::GrammarSchemaData&& d) noexcept;

    // ── Loaders ──
    static LoadResult<std::shared_ptr<GrammarSchema>> loadFromFile(
        std::filesystem::path const& path);

    static LoadResult<std::shared_ptr<GrammarSchema>> loadShipped(std::string_view name);

    static LoadResult<std::shared_ptr<GrammarSchema>> loadFromText(
        std::string_view jsonText,
        std::string_view sourceLabel = "<inline>");

    // ── Introspection ──
    [[nodiscard]] std::string_view             name()           const noexcept { return d_.name; }
    [[nodiscard]] std::string_view             version()        const noexcept { return d_.version; }
    [[nodiscard]] std::uint32_t                schemaVersion()  const noexcept { return d_.schemaVersion; }
    [[nodiscard]] RuleInterner const&          rules()          const noexcept { return *d_.rules; }
    [[nodiscard]] SchemaTokenInterner const&   schemaTokens()   const noexcept { return *d_.schemaTokens; }
    [[nodiscard]] std::span<std::string const> fileExtensions() const noexcept { return d_.fileExtensions; }

    // ── Token recognition ──
    [[nodiscard]] std::span<LexemeMeaning const> lookupLexeme(std::string_view lexeme) const noexcept;
    [[nodiscard]] bool isEmptySpace(SchemaTokenId id) const noexcept;

    // ── Shape navigation ──
    [[nodiscard]] SchemaCursor rootCursor() const noexcept;
    [[nodiscard]] std::span<std::string const> expectedAt(SchemaCursor cur) const noexcept;

    // ── Scope rules ──
    [[nodiscard]] bool isTokenValidInScope(SchemaTokenId tok,
                                           std::span<ScopeKind const> stack) const noexcept;

    // ── Termination ──
    [[nodiscard]] bool canEndSource(SchemaCursor cur) const noexcept;

private:
    detail::GrammarSchemaData d_;
};

} // namespace dss
