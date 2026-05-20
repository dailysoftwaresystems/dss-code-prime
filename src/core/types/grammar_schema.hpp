#pragma once

#include "core/export.hpp"
#include "core/types/compiled_shape.hpp"
#include "core/types/lexer_mode.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/schema_cursor.hpp"
#include "core/types/schema_token_interner.hpp"
#include "core/types/scope_kind.hpp"
#include "core/types/string_style.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree_node.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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

// Per-meaning scope-stack constraints. Every field defaults to "no
// constraint": empty `anyOf`/`forbid` and unset `topMustBe`/`outermost`
// all mean "no requirement from this axis." Legacy `validScopes: [...]`
// loads as `scopeRequire.anyOf` (backward compat).
//
// Enforcement: `meaningAllowedByScopeRequire` in `tree_builder.cpp` is
// the canonical site that documents and applies the check order.
//
// Lifetime: `anyOf` and `forbid` are `std::span`s into
// `GrammarSchemaData::scopeListPool`. The loader reserves the pool up
// front based on a Pass-A count so subsequent `push_back`s never
// reallocate; the spans remain valid for the lifetime of the owning
// `GrammarSchema`. Mutating the pool after construction is unsupported.
struct DSS_EXPORT ScopeMatch {
    std::span<ScopeKind const>  anyOf;
    std::span<ScopeKind const>  forbid;
    std::optional<ScopeKind>    topMustBe;
    std::optional<ScopeKind>    outermost;
};

static_assert(std::is_trivially_copyable_v<ScopeMatch>,
              "ScopeMatch must stay trivially copyable — copied through "
              "the candidate-filtering hot path in resolveMeaning.");

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
    // Soft keyword: outside the cursor's expectedSet, degrades to
    // Identifier. Set per-keyword (`contextual: true`) or by policy
    // (`reservedWordPolicy: "contextual"`).
    bool             contextual    = false;
    ScopeMatch       scopeRequire{};
    // Lexer-mode-stack effect applied by the tokenizer after this
    // meaning is produced. `modeArg` is the target mode for
    // Push/Replace (ignored for Pop); resolved at load time so the
    // tokenizer doesn't re-walk mode-name strings per token.
    ModeOp           modeOp        = ModeOp::None;
    LexerModeId      modeArg{};
    // Strong id into `GrammarSchemaData::stringStyles` when this meaning
    // opens a delimited string (e.g. `"`, `@"`, `R"`). Default-invalid
    // for every other meaning. Pool-indexed (not embedded) so
    // LexemeMeaning stays trivially copyable.
    StringStyleId    stringStyleId{};
    // Identifies the schema that owns this meaning. Stamped by the
    // loader at construction; consumed by `GrammarSchema::stringStyle()`
    // and similar lookups to catch cross-schema misuse where a caller
    // copies a meaning out of schema A and queries schema B.
    SchemaId         schemaId{};
};

static_assert(std::is_trivially_copyable_v<LexemeMeaning>,
              "LexemeMeaning must stay trivially copyable — copied through "
              "the candidate-filtering hot path in resolveMeaning.");

// How aggressively the builder treats keywords as reserved.
//
//   Strict     — every keyword always wins over Identifier (the default;
//                what every config without a `reservedWordPolicy` field
//                gets).
//   Contextual — every keyword degrades to Identifier when not in the
//                cursor's `expectedSet`. Used by languages like T-SQL
//                where any keyword may also appear as a plain identifier.
enum class ReservedWordPolicy : std::uint8_t {
    Strict,
    Contextual,
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

    // Backing storage for ScopeMatch.anyOf / .forbid spans. Reserved up
    // front by the loader so no reallocation occurs — the spans inside
    // LexemeMeaning::scopeRequire point into stable storage.
    std::vector<std::vector<ScopeKind>>               scopeListPool;

    // O(1) "is this token EmptySpace?" without scanning lexemeTable.
    std::unordered_set<std::uint32_t>                 emptySpaceTokens;

    // Per-scope forbidden-token sets — keyed by ScopeKind's underlying
    // value, value = set of SchemaTokenId values.
    std::unordered_map<std::uint16_t, std::unordered_set<std::uint32_t>> scopeForbid;

    // Root rule's id (the "root" shape from config) — anchors rootCursor().
    RuleId rootRule = InvalidRule;

    // Per-rule compiled shape — populated by the loader after shape-reference
    // validation. Indexed by RuleId.v. Position[0] in every rule's table is
    // the "invalid sentinel" so 0 is reserved as the invalid posId in
    // SchemaCursor.
    std::unordered_map<std::uint32_t, CompiledRule> compiledRules;

    // Operator precedence + associativity by (SchemaTokenId, arity).
    // Empty when the config has no `operators` section. Read-only after
    // construction; the loader is the only writer.
    OperatorTable operators;

    // Default Strict — every keyword is reserved. `reservedWordPolicy:
    // "contextual"` in JSON flips this to Contextual, and the loader
    // also forces `contextual = true` on every keyword's LexemeMeaning.
    ReservedWordPolicy reservedWordPolicy = ReservedWordPolicy::Strict;

    // Lexer-mode tables. `lexerModes[0]` is the InvalidLexerMode
    // sentinel — DO NOT iterate this vector directly from outside
    // the loader. Use `GrammarSchema::lexerModes()` which hides the
    // sentinel via `subspan(1)`. Real ids dense 1..N; "main"
    // synthesized at id 1 even when JSON omits `lexerModes`.
    // `lexerModeTokens` keyed by id; "main" mirrors `lexemeTable`;
    // modes with `tokens: "default"` inherit it; inline
    // `tokens: {...}` parsing is deferred (loader warns).
    std::vector<LexerMode>                            lexerModes;
    std::unordered_map<std::string, LexerModeId>      lexerModeIds;
    std::unordered_map<std::uint32_t,
                       std::unordered_map<std::string,
                                          std::vector<LexemeMeaning>>>
                                                      lexerModeTokens;

    // Pool indexed by `LexemeMeaning::stringStyleId`. Slot 0 is the
    // InvalidStringStyle sentinel; real ids 1..N. Each StringStyle
    // owns its `endsAt`/`tagPattern` strings; the vector may reallocate
    // (LexemeMeaning carries the id, not a pointer).
    std::vector<StringStyle>                          stringStyles;

    // Per-instance monotonic id stamped onto every LexemeMeaning so
    // cross-schema lookups (`stringStyle(m)` etc.) catch the case where
    // `m` was copied out of a different schema. Allocated by the loader
    // before any LexemeMeaning is populated.
    SchemaId                                          id{};

    // Longest declared lexeme key, in bytes. Computed by the loader
    // once `lexemeTable` is finalized; consumed by the tokenizer to
    // cap its longest-match probe length. Zero only for a schema with
    // no declared `tokens` entries.
    std::size_t                                       maxLexemeLength = 0;
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

    // Longest declared lexeme key in bytes — used by the tokenizer to
    // bound its longest-match probe length so 5+ char lexemes can't
    // silently truncate. Computed at load time; zero only for an
    // empty lexemeTable.
    [[nodiscard]] std::size_t maxLexemeLength() const noexcept { return d_.maxLexemeLength; }

    // ── Operators ──
    [[nodiscard]] OperatorTable const& operatorTable() const noexcept { return d_.operators; }

    // ── Reserved-word policy ──
    [[nodiscard]] ReservedWordPolicy reservedWordPolicy() const noexcept {
        return d_.reservedWordPolicy;
    }

    // The compiled mode table. Always non-empty (synthesized "main"
    // mode for v1 configs). The returned span hides the internal
    // index-0 sentinel — every visible element is a real declared
    // or synthesized mode.
    [[nodiscard]] std::span<LexerMode const> lexerModes() const noexcept;

    // Lookup a mode by name. Returns InvalidLexerMode if not found.
    [[nodiscard]] LexerModeId findLexerMode(std::string_view name) const noexcept;

    // Lookup a mode by id. Aborts via the strong-id contract if `id`
    // doesn't refer to a real mode in this schema.
    [[nodiscard]] LexerMode const& lexerMode(LexerModeId id) const noexcept;

    // Per-mode lexeme lookup. Empty span when the mode has no entries
    // for `lexeme`. Aborts on `InvalidLexerMode` or out-of-range id —
    // matches `lexerMode(id)`'s strong-id contract so an empty span
    // always means "no meanings for this lexeme," never "wrong id."
    [[nodiscard]] std::span<LexemeMeaning const>
        lookupLexemeInMode(LexerModeId mode, std::string_view lexeme) const noexcept;

    // Per-instance schema id stamped onto every owned `LexemeMeaning`.
    // Used by accessors like `stringStyle(m)` to assert that `m`
    // actually came from this schema rather than a copy from another.
    [[nodiscard]] SchemaId schemaId() const noexcept { return d_.id; }

    // String-literal metadata for a meaning that opens a delimited
    // string body. Returns nullptr when the meaning has no `stringStyle`
    // declared (the common case). Aborts on (a) an `m.schemaId` that
    // doesn't match this schema (cross-schema misuse) or (b) an out-of-
    // range id (corrupted meaning). Both are caller bugs; the abort
    // surfaces them at the failing call site rather than letting the
    // wrong-but-plausible StringStyle propagate silently.
    [[nodiscard]] StringStyle const* stringStyle(LexemeMeaning const& m) const noexcept;

    // ── Shape navigation ──
    //
    // SchemaCursor is a per-rule position. Descent into nested rules is
    // caller-managed via a stack of cursors. `advance` consumes a token
    // at the current step (TokenLeaf or AltChoice slots). `enterRule`
    // returns a fresh cursor at the start of the named rule — the caller
    // saves the parent cursor so it can resume the parent via `leaveRule`
    // once the child reaches end-of-body.
    //
    // `advance` returns an invalid cursor (`valid() == false`) on either
    // a token mismatch OR when the current slot is `RuleLeaf` / `End`
    // (the caller must `enterRule` / `leaveRule` for those). Inspect via
    // `slotKind` before calling `advance` if the distinction matters.

    [[nodiscard]] SchemaCursor rootCursor() const noexcept;
    [[nodiscard]] SchemaCursor enterRule(RuleId rule) const noexcept;
    [[nodiscard]] SchemaCursor leaveRule(SchemaCursor parentCur) const noexcept;
    [[nodiscard]] SchemaCursor advance(SchemaCursor cur, SchemaTokenId tok) const noexcept;

    // Walk `parentCur` through any AltChoice positions to find a RuleLeaf
    // slot for `rule`, returning that RuleLeaf cursor. Used by builders
    // that want to save a saved-parent cursor for `leaveRule` symmetry
    // when the parent slot is an AltChoice (e.g. the body of a `repeat`
    // or an `optional`/`alt` whose chosen branch is RuleLeaf(rule)).
    //
    // Returns `parentCur` unchanged when it's already at RuleLeaf(rule).
    // Returns an invalid cursor when no path through AltChoice positions
    // leads to a RuleLeaf for `rule` — caller falls back to saving the
    // original cursor (and leaveRule will then report off-track).
    [[nodiscard]] SchemaCursor routeToRuleLeaf(SchemaCursor parentCur,
                                               RuleId rule) const noexcept;

    [[nodiscard]] std::span<SchemaTokenId const> expectedSet(SchemaCursor cur) const noexcept;

    [[nodiscard]] SlotKind slotKind(SchemaCursor cur) const noexcept;
    [[nodiscard]] RuleId   slotRuleRef(SchemaCursor cur) const noexcept;
    [[nodiscard]] bool     isAtEndOfRule(SchemaCursor cur) const noexcept;

    // Speculative-alt attributes attached to AltChoice slots by configs
    // declaring `"speculative": true`. Both return defaults (`false`, 0)
    // for non-AltChoice slots OR non-speculative alts. The cursor walker
    // does not consume these — they're stored for the future parser to
    // read when deciding whether to take a `TreeBuilder::Checkpoint`
    // before exploring a branch.
    [[nodiscard]] bool         isSpeculativeAlt(SchemaCursor cur) const noexcept;
    [[nodiscard]] std::uint16_t lookahead(SchemaCursor cur) const noexcept;

    // Direct per-rule queries that don't require a cursor instance.
    [[nodiscard]] std::span<SchemaTokenId const> firstSetOf(RuleId rule) const noexcept;
    [[nodiscard]] bool                           isNullable(RuleId rule) const noexcept;

    // ── Scope rules ──
    [[nodiscard]] bool isTokenValidInScope(SchemaTokenId tok,
                                           std::span<ScopeKind const> stack) const noexcept;

    // ── Termination ──
    [[nodiscard]] bool canEndSource(SchemaCursor cur) const noexcept;

private:
    detail::GrammarSchemaData d_;
};

} // namespace dss
