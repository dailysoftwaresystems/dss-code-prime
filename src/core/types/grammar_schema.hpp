#pragma once

#include "core/export.hpp"
#include "core/types/compiled_shape.hpp"
#include "core/types/config_document_memo.hpp"  // ConfigDocumentDependency — the referenced-document ledger
#include "core/types/import_config.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/lexer_mode.hpp"
#include "core/types/type_lattice/core_type.hpp"  // TypeExtensionDescriptor
#include "core/types/number_style.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/enum_name_table.hpp"  // EnumNameTable (kReservedWordPolicyTable)
#include "core/types/parse_diagnostic.hpp"
#include "core/types/hir_lowering_config.hpp"
#include "core/types/assembly_config.hpp"
#include "core/types/pipeline_entry_config.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/schema_cursor.hpp"
#include "core/types/schema_token_interner.hpp"
#include "core/types/scope_kind.hpp"
#include "core/types/string_style.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree_node.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
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

// ★★★ THE PER-LANGUAGE IDENTIFIER CHARACTER CLASS (schema v4
// `identifierClass`) — D-ASM-DIALECT-IDENTIFIER-CONTINUATION-NOT-CONFIGURABLE,
// 2026-08-13. Until this existed the tokenizer's identifier rule was a pair of
// hardcoded `constexpr` predicates (`isIdStart` / `isIdContinue` in
// `tokenizer.cpp`), so a language whose names legitimately contain a character
// outside `[A-Za-z0-9_]` had exactly one way to spell them: declare each whole
// name as a LEXEME in the `tokens` map.
//
// ✔MEASURED, and this is the cost that forced the facet: `aarch64-linux-gnu-as`
// accepts `b.eq` (one mnemonic — you cannot write `b . eq`) and `foo.bar:` (one
// symbol), both rc=0. `.` is the arm64 dialect's `directiveIntroducer`, so
// without a continuation rule the tokenizer produced Identifier(`b`) +
// DirectiveDot + Identifier(`eq`) and the line was a parse error. The dialect's
// workaround was to declare all TWELVE conditional-branch spellings TWICE — once
// as a `tokens` lexeme and once as an `instructions[]` row — with the two
// required to agree. Both mismatch directions failed loud, so nothing was
// silent, but the duplication is exactly the shape this project keeps closing.
//
// ★★ IT DECLARES CONTINUATION ONLY, NEVER START, AND THAT ASYMMETRY IS THE
// DESIGN RATHER THAN AN OMISSION. gas lets `.` both start and continue a
// symbol; DSS reaches the leading `.` through the `directiveIntroducer` TOKEN
// plus `asmDirective`'s `{optional asmLabelTail}` slot, which is what already
// makes `.L3:` a label and `.text` a directive — two constructs that are
// byte-identical up to the token AFTER the name. Adding an `extraStart` would
// mint a SECOND mechanism for the same byte, and the two would then have to
// agree about which of `.L3:` / `.text` / `.section` each owns. One mechanism
// per question: a leading `.` is the introducer token; an interior `.` is this.
// ⇒ An `extraStart` key is NOT accepted — it is not a gap awaiting a follow-up,
// and a language needing one is the trigger to re-derive the question, not to
// widen this struct.
//
// ★ ADDITIVE ONLY: the universal `[A-Za-z0-9_]` + UTF-8 continuation set stays
// in force and cannot be REMOVED by config. Nothing measured needs subtraction,
// and a language that could delete `_` from its own identifiers would break
// every lexeme lookup in its own `tokens` map with no diagnostic.
struct DSS_EXPORT IdentifierClass {
    // Extra characters that CONTINUE an identifier once one has started, as a
    // character-class string in the SAME syntax `numberStyle`'s `digits` uses
    // (literal characters and `a-z` ranges) — matched by the shared
    // `digitClassMatches` helper in `number_style.hpp`, which its own docblock
    // already names as the single source for "does this character land in this
    // class". EMPTY (the default, and the value for every language declaring no
    // block) means the universal rule and nothing more.
    std::string extraContinue;

    [[nodiscard]] bool declared() const noexcept {
        return !extraContinue.empty();
    }

    // ★★★ THE UNIVERSAL RULE, AND THE ONE PLACE IT IS WRITTEN DOWN. It lived as
    // a file-local `constexpr` in `tokenizer.cpp` until this facet needed a
    // SECOND reader: the loader has to know which characters ALREADY continue an
    // identifier, so it can refuse an `extraContinue` that silently declares
    // nothing. Two copies of "what continues a name" is how the loader starts
    // accepting a class the tokenizer ignores — the knob-that-lies shape,
    // arrived at from the validation side instead of the consumption side.
    //
    // ASCII letters, digits and `_`, plus every byte ≥ 0x80: the tokenizer is
    // byte-oriented, and a multi-byte UTF-8 run must not terminate mid-sequence
    // (its LEAD byte is what `isIdStart` gates on).
    [[nodiscard]] static constexpr bool universalContinue(char c) noexcept {
        const auto u = static_cast<unsigned char>(c);
        return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_'
            || (u >= '0' && u <= '9')
            || u >= 0x80;
    }

    // The full question the tokenizer asks per identifier byte: the universal
    // rule OR this language's declared extras. ⚠ ADDITIVE BY CONSTRUCTION —
    // there is no arm in which a declaration makes `universalContinue` stop
    // holding, so no config can delete `_` from its own identifiers and break
    // every lexeme lookup in its own `tokens` map with no diagnostic.
    [[nodiscard]] bool continuesIdentifier(char c) const noexcept {
        return universalContinue(c)
            || (!extraContinue.empty()
                && digitClassMatches(extraContinue, c));
    }
};

// Pratt-walker wrapper rule names per `expr` shape (schema v4
// `expr.wrapperRules`). Each `expr`-kind rule declares the three
// names the walker will synthesize around operator-precedence
// results — the engine no longer hardcodes `binaryExpr`/`unaryExpr`/
// `postfixExpr`. The loader auto-interns the declared names and
// stores their RuleIds here; the parser's Pratt walker reads
// `GrammarSchema::exprWrapperRules(exprRule)` once per
// walkExpression entry.
struct DSS_EXPORT ExprWrapperRules {
    RuleId binary;
    RuleId unary;
    RuleId postfix;
    // OPTIONAL fourth wrapper — the mixfix `ternary` rule (`cond ? then : else`).
    // Invalid for languages without a ternary; when valid it must be distinct
    // from the other three. Kept optional so the three required wrappers (the
    // valid() contract) are unaffected by a language that has no ternary.
    RuleId ternary;

    // The three REQUIRED RuleIds are valid AND pairwise-distinct. The walker
    // tags frames by RuleId; a duplicate id would collide silently
    // (e.g. both `binary` and `unary` interned to the same name and
    // RuleId — Pratt frames meant for two different climb shapes
    // would land in one bucket). The loader rejects duplicates up
    // front (see `C_MissingWrapperRules` in grammar_schema_json.cpp);
    // this predicate is the runtime safety net. `ternary` is optional and not
    // part of this contract (checked separately by the loader when present).
    [[nodiscard]] bool valid() const noexcept {
        return binary.valid() && unary.valid() && postfix.valid()
            && binary.v != unary.v
            && unary.v  != postfix.v
            && binary.v != postfix.v;
    }
};

static_assert(std::is_trivially_copyable_v<ExprWrapperRules>,
              "ExprWrapperRules must stay trivially copyable — read once "
              "per walkExpression call and copied by value into the Pratt "
              "wrapper bundle.");

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

// ── THE SPELLINGS HAVE ONE OWNER (D-CONFIG-GRAMMAR-LOADER-INLINE-CHAIN-VOCABULARIES-REMAIN) ──
//
// The document-level `reservedWordPolicy` key, previously owned by an inline
// `v == "strict" / "contextual"` chain in the grammar loader with two sentences
// beside it restating the pair. `Strict` is row 0, matching the default a
// document without the key gets.
inline constexpr EnumNameTable<ReservedWordPolicy, 2> kReservedWordPolicyTable{{{
    { ReservedWordPolicy::Strict,     "strict"     },
    { ReservedWordPolicy::Contextual, "contextual" },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kReservedWordPolicyTable);

[[nodiscard]] constexpr std::string_view
reservedWordPolicyName(ReservedWordPolicy p) noexcept {
    return kReservedWordPolicyTable.name(p);
}
[[nodiscard]] constexpr std::optional<ReservedWordPolicy>
reservedWordPolicyFromName(std::string_view s) noexcept {
    return kReservedWordPolicyTable.fromName(s);
}

// Standard C++23 fallible result. Error channel is the full list of
// diagnostics collected before bailing — the loader keeps walking to
// surface as many problems as possible per run.
template <typename T>
using LoadResult = std::expected<T, std::vector<ConfigDiagnostic>>;

namespace detail {

// ── lexeme tables ─────────────────────────────────────────────────────────
//
// D-PERF-TOK-LONGEST-MATCH-PROBES-EVERY-DECLARED-LENGTH-AT-EVERY-POSITION,
// half one of two. The tokenizer probes a lexeme table with the first `len`
// bytes of the remaining input; before this hasher those tables were keyed
// by `std::string`, so `lookupLexeme` had to MATERIALISE a `std::string`
// from the probe bytes for every single probe — a copy always, and a
// malloc/free pair once the probe passed libstdc++'s 15-byte small-string
// threshold. C++20 heterogeneous lookup removes both: with a transparent
// hash and a transparent equality, `find` accepts the `std::string_view`
// the tokenizer already holds and allocates nothing.
//
// ★ THE HASH VALUE IS DELIBERATELY UNCHANGED, and that is a correctness
// property, not a nicety. [basic.string.hash] REQUIRES
// `hash<string>()(s) == hash<string_view>()(string_view(s))`, so every key
// lands in the bucket it landed in before, the table's iteration order is
// byte-identical, and the handful of loader passes that walk `lexemeTable`
// unordered keep seeing it in exactly the former order. (One of them —
// the `hirLowering.stringDoubledDelimiter` derivation — is last-write-wins
// over that walk, so a reordering there would silently change a shipped
// grammar's string delimiter.)
struct LexemeHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

// lexeme → declared meanings, in priority-ascending order (stable —
// declaration order wins on ties).
using LexemeTable =
    std::unordered_map<std::string, std::vector<LexemeMeaning>,
                       LexemeHash, std::equal_to<>>;

// ── per-lead-byte declared-length index ───────────────────────────────────
//
// D-PERF-TOK-LONGEST-MATCH-PROBES-EVERY-DECLARED-LENGTH-AT-EVERY-POSITION,
// half two of two. A longest-match scan used to ask a lexeme table about
// EVERY length from `maxLexemeLength` down to 1 at EVERY token position.
// For the shipped `c` grammar `maxLexemeLength` is 28 — keywords share the
// table with punctuation and `__builtin_types_compatible_p` is 28 bytes —
// so a `+` paid 28 hash lookups to discover that exactly two lengths have
// any key starting with `+` at all.
//
// This is the derived index that removes the guaranteed misses: for each
// lead byte, the LENGTHS at which some key in this table starts with that
// byte, descending. A probe iterates only those; a lead byte no key starts
// with (206 of 256 for `c`) skips the scan entirely. It is EXACT, not
// heuristic — a length absent from a lead byte's row cannot match, because
// every candidate substring starts with that byte — so the winning length
// is unchanged at every position and the token stream is byte-identical.
//
// ⚠ NOT A CACHE. Nothing here remembers a past answer; it is a projection
// of the table's own declared keys, computed once when the schema is
// sealed and immutable thereafter. It is 100% config-derived and names no
// language, target, or byte class — every length in it came out of a key the
// config declared, so a grammar whose longest key is 3 bytes gets rows that
// stop at 3, and one with a 400-byte key gets a row containing 400.
//
// ★ CSR, NOT A 64-BIT MASK. The obvious encoding is one `std::uint64_t`
// per lead byte with bit L set for length L, and it was rejected: it
// cannot represent a key longer than 63 bytes, so it needs a
// "`maxLexemeLength > 63` ⇒ silently probe everything" fallback — a
// perf cliff no test can see and no diagnostic reports. Offsets + a flat
// descending length array has no representable-length limit at all, so
// there is no fallback arm to get wrong, and the hot loop is a plain
// forward walk over 1-3 contiguous `std::uint32_t`s instead of a
// bit-scan.
class DSS_EXPORT LexemeLengthIndex {
public:
    // Derive the index from `table`. Idempotent; safe to call on a
    // default-constructed index (which answers "no key starts with any
    // byte" — correct for an ABSENT table, e.g. a lexer mode that
    // declares no `tokens` override, whose lookups all miss anyway).
    void build(LexemeTable const& table);

    // Lengths, DESCENDING, at which some key in this table starts with
    // `lead`. Empty when none does.
    [[nodiscard]] std::span<std::uint32_t const>
    lengthsFor(unsigned char lead) const noexcept {
        return std::span<std::uint32_t const>(lengths_)
            .subspan(rowStart_[lead],
                     rowStart_[std::size_t{lead} + 1] - rowStart_[lead]);
    }

    // Total number of (lead byte, length) pairs — i.e. the number of table
    // lookups a longest-match scan performs summed over every possible lead
    // byte. THE algorithmic figure this index exists to shrink, exposed so a
    // test can pin it without timing anything.
    [[nodiscard]] std::size_t probeCount() const noexcept { return lengths_.size(); }

private:
    // `rowStart_[b] .. rowStart_[b+1]` slices `lengths_`. All-zero when
    // unbuilt, which yields an empty row for every byte.
    std::array<std::uint32_t, 257> rowStart_{};
    std::vector<std::uint32_t>     lengths_;
};

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
    // declaration order wins on ties). See `LexemeTable` for why the
    // hasher is transparent.
    LexemeTable                                       lexemeTable;

    // Backing storage for ScopeMatch.anyOf / .forbid spans. Reserved up
    // front by the loader so no reallocation occurs — the spans inside
    // LexemeMeaning::scopeRequire point into stable storage.
    std::vector<std::vector<ScopeKind>>               scopeListPool;

    // O(1) "is this token EmptySpace?" without scanning lexemeTable.
    std::unordered_set<std::uint32_t>                 emptySpaceTokens;

    // Every SchemaTokenId that some `tokens` (or per-mode `tokens`) entry
    // DECLARES. ★ IT IS THE COMPANION OF `emptySpaceTokens`, NOT A DUPLICATE:
    // that set answers "is this token trivia?", this one answers "did the
    // LANGUAGE say anything about this token at all?" — and without the second
    // question the first cannot be trusted, because a built-in kind the schema
    // never mentioned is absent from `emptySpaceTokens` for the same reason a
    // deliberately-significant one is. The parser needs to tell "declared and
    // NOT trivia" from "never declared", since only the first may override the
    // core-kind default (see `isSkippableTrivia`).
    std::unordered_set<std::uint32_t>                 declaredLexemeTokens;

    // D-PARSE-PREDICTIVE-PRUNE-CONTEXTUAL-KEYWORD: the set of SchemaTokenId
    // values that are CONTEXTUAL / scope-resolvable — a soft keyword that the
    // builder may DEMOTE to Identifier outside the cursor's expectedSet (a
    // `contextual: true` LexemeMeaning, or — under `reservedWordPolicy:
    // "contextual"` — every keyword, which the loader marks contextual). Derived
    // at load by `computeContextualKinds` from the `contextual` flags. The LL(k)
    // predictive prune (`Parser::predictivePrefixPrunes`) SKIPS any offset whose
    // observed token is in this set, so it never wrongly prunes a candidate the
    // demoted token would match. Token-id-keyed (the `contextual` flag is
    // per-lexeme-string in `lexemeTable`; this is the parser-side query).
    std::unordered_set<std::uint32_t>                 contextualKinds;

    // D-C-ATTRIBUTE-CLAUSE-NAME-ADMITS-ONLY-IDENTIFIER-SO-A-KEYWORD-NAMED-ATTRIBUTE-IS-REFUSED:
    // the document's declared TOKEN CLASSES — `tokenClasses.<name>` maps a name
    // to a SET of token kinds, and that ONE declaration is what both the GRAMMAR
    // (`{"tokenClass": "<name>"}` as a shape element) and the SEMANTIC tier
    // (`semantics.attributeSemantics.clauseNameTokenClass`) resolve against.
    //
    // ★ THE SHARED DECLARATION IS THE POINT, not a convenience. The row this
    // closes exists because a grammar position and a semantic reader BOTH decided
    // "is this token a name?" and decided it separately — widen one and the other
    // silently drops what the first now admits. Two lists cannot drift when there
    // is one list; a per-side spelling could, and did.
    //
    // Values are SORTED + deduplicated by the loader (the shape builder hands
    // them straight to `Position::makeTokenClassLeaf`, whose `expectedSet`
    // contract is a sorted set). A class that resolves to the EMPTY set is a load
    // error, never a silently-never-matching slot.
    std::unordered_map<std::string, std::vector<SchemaTokenId>> tokenClasses;

    // Per-scope forbidden-token sets — keyed by ScopeKind's underlying
    // value, value = set of SchemaTokenId values.
    std::unordered_map<std::uint16_t, std::unordered_set<std::uint32_t>> scopeForbid;

    // Root rule's id (the "root" shape from config) — anchors rootCursor().
    RuleId rootRule = InvalidRule;

    // Per-rule compiled shape — populated by the loader after shape-reference
    // validation. Indexed by RuleId.v. Position[0] in every rule's table is
    // the "invalid sentinel" so 0 is reserved as the invalid posId in
    // SchemaCursor.
    //
    // c97: this map is the LOADER's build-time container only. The
    // GrammarSchema ctor DRAINS it into a dense `std::vector<CompiledRule>`
    // indexed by RuleId.v (RuleIds are dense interner ids) so every
    // per-token grammar query is an array index instead of a hash lookup —
    // after construction this map is empty and never read.
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
    // modes with `tokens: "default"` inherit it; an inline
    // `tokens: {...}` object is parsed into a per-mode override table,
    // consulted BEFORE `lexemeTable` with global fallback (see
    // `lookupLexemeInMode`) — context-sensitive lexing.
    std::vector<LexerMode>                            lexerModes;
    std::unordered_map<std::string, LexerModeId>      lexerModeIds;
    std::unordered_map<std::uint32_t, LexemeTable>    lexerModeTokens;

    // Off-grammar body-token kinds — see
    // `GrammarSchema::bodyDefaultTokenKinds()` for the contract.
    std::unordered_set<SchemaTokenId>                 bodyDefaultTokenKinds;

    // Mode-introduced token kinds (FF11): kinds the tokenizer may
    // pre-resolve from a NON-MAIN mode context that have NO entry in the
    // GLOBAL per-lexeme table for the matched lexeme — i.e. (a) kinds
    // declared in a per-mode `tokens` override (e.g. `HeaderStart`, the
    // in-`include-directive` meaning of `<`, distinct from the global
    // `<`→LtOp) and (b) COALESCED `defaultToken` kinds that aren't
    // built-in literals (e.g. `HeaderPath`). The builder's
    // `resolveMeaning` consults only the global table (it doesn't track
    // lexer modes — the tokenizer is authoritative about mode context),
    // so without this set a legitimately mode-introduced kind would trip
    // the synthetic-meaning DRIFT guard. Populated by the loader once;
    // queried via `isModeIntroducedKind`.
    std::unordered_set<SchemaTokenId>                 modeIntroducedKinds;

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

    // Config-driven parser expression-nesting cap (optional top-level
    // `parser.maxExpressionDepth`, additive — every schema version may
    // declare it). `nullopt` when the config omits the field — the
    // driver then leaves `ParserConfig::maxExpressionDepth` at its C++
    // fallback default (256). When present, the loader validated it is a
    // positive integer; the CU build copies it onto the `ParserConfig`
    // for every parse of this language so the cap is 100% config-driven,
    // not a hardcoded engine constant. The parser keeps `P_ExpressionTooDeep`
    // as the fail-loud backstop at WHATEVER this value is — a nest beyond
    // it still fails loud, never crashes. See `maxExpressionDepth()`.
    std::optional<std::size_t>                        maxExpressionDepth;

    // Config-driven parser SPECULATION-nesting cap (optional top-level
    // `parser.maxSpeculationDepth`, additive — every schema version may
    // declare it). `nullopt` when the config omits the field — the driver
    // then leaves `ParserConfig::maxSpeculationDepth` at its C++ fallback
    // default. When present, the loader validated it is a positive integer;
    // the CU build copies it onto the `ParserConfig` for every parse of this
    // language, and the parser in turn DERIVES the `TreeBuilder`'s
    // `BuilderConfig::maxSpeculationDepth` from the same value — the two are
    // the same nesting quantity measured at two layers (every parser
    // speculation probe opens exactly one builder checkpoint), so ONE key
    // drives both and they cannot disagree. A nest beyond it fails loud with
    // a positioned, self-naming `P_MaxSpeculationDepth`, never a fabricated
    // syntax error against the user's own token
    // (D-PARSE-NINE-NESTED-CASTS-ARE-REFUSED-BY-THE-SPECULATION-CAP-WITH-A-FABRICATED-SYNTAX-ERROR).
    // See `maxSpeculationDepth()`.
    std::optional<std::size_t>                        maxSpeculationDepth;

    // Config-driven multiplier for the per-probe speculative TOKEN budget
    // (optional top-level `parser.speculationBudgetFactor`, additive).
    // `nullopt` when the config omits the field — the driver then leaves
    // `ParserConfig::speculationBudgetFactor` at its C++ fallback. One
    // speculative probe may consume `factor x <the alt's declared lookahead>`
    // tokens before it is abandoned. This is the THIRD ceiling on a nested
    // construct, behind the two speculation-depth caps, and until it became a
    // config key it was a bare `16` in the probe constructor that silently
    // truncated the admissible language: a cast chain of 342 was refused where
    // 341 compiled, with no diagnostic naming the budget at all. It now fails
    // loud as `P_SpeculationBudgetExhausted`. See `speculationBudgetFactor()`.
    std::optional<std::size_t>                        speculationBudgetFactor;

    // Panic-mode sync tokens declared at the schema level — token
    // kinds the parser treats as "safe resync points" when the input
    // is broken. Sorted ascending by `id.v` so callers can use
    // binary-search probes. Loader-validated: every entry must be a
    // declared token kind, and Eof/Error are rejected (Eof is always
    // an implicit sync; Error would short-circuit recovery).
    std::vector<SchemaTokenId>                        syncTokens;

    // Per-language type-extension declarations (SP2; `typeExtensions[]`,
    // additive in schema v3). Empty for v1/v2 configs. Registered into a CU's
    // TypeRegistry at CU build time via registerSchemaTypeExtensions.
    std::vector<TypeExtensionDescriptor>              typeExtensions;

    // Artifact profiles this language supports (plan 06 AP1; optional
    // top-level `artifactProfiles[]`, additive in schema v4). Each entry is
    // a registered profile name (cli/gui/lib/staticlib/script/sproc/
    // transpile/shader/hdl). Empty when the field is absent. AP1 is the
    // schema-field + loader-validation slice ONLY — no codegen/driver
    // consumes it yet; the driver-enforcement (AP2+) reads this set to
    // reject a project asking for an unsupported profile.
    std::vector<std::string>                          artifactProfiles;

    // ★★★ THE INSTRUCTION-SET ARCHITECTURE THIS LANGUAGE EMITS — the LANGUAGE
    // half of the (language emits · target executes) ISA axis, optional
    // top-level `isa` (D-ISA-LANGUAGE-BOUND-TO-ARCHITECTURE). The target half
    // is `TargetSchema::isa()`; the gate compares the two DECLARED strings.
    //
    // ★ EMPTY IS THE DEFAULT AND IT MEANS **PORTABLE**, NOT "unknown". Most
    // source languages compile for any architecture — C, T-SQL, the toy
    // language, and the shared `asm` inline-asm core all declare nothing and
    // build for every target, paying nothing for this axis existing. Only a
    // language whose surface IS an instruction set declares a value:
    // `asm-x86_64-att` cannot be assembled for AArch64 and `asm-arm64-gas`
    // cannot be assembled for x86-64, and that is DEFINITIONAL — it is what
    // those languages ARE, not a list of platforms someone remembered to
    // maintain.
    //
    // ⚠ THIS IS NOT A CAPABILITY LIST, AND THE DISTINCTION IS THE ONE THAT
    // KILLED THE EARLIER DESIGN. A per-project enumeration of supported
    // targets was REJECTED (2026-08-14) because `targets[]` is "the platforms
    // a project builds for ITSELF" — a BUILD LIST that drifts, so reading it
    // as a capability claim makes a portable dependency that merely forgot to
    // list an architecture refuse a legitimate consumer. A dialect's ISA
    // cannot drift that way: it is a single fact about the language, and a
    // NEW target declaring the same `isa` satisfies an existing binding with
    // NO edit to this or any other language document.
    //
    // Compared by EQUALITY ONLY — deliberately no subset/superset lattice.
    // "x86-64 code also runs in a 32-bit x86 process" is exactly the kind of
    // capability claim this axis refuses to make; a language that genuinely
    // emits for two architectures is two languages (which is why the shipped
    // dialects are already named `asm-<arch>-<syntax>`).
    std::string                                       isa;

    // Config-driven import resolution (schema v4 `imports` block). Default
    // `ImportStrategy::None` (no cross-refs) for v1/v2/v3 configs and any v4
    // config that omits the block. Consumed by ConfigDrivenImportResolver —
    // the single language-agnostic import engine.
    ImportConfig                                      imports;

    // Config-driven C-preprocessor (schema v4 `preprocess` block; FC13).
    // Default `enabled == false` (no block) -> the preprocess pass is a
    // strict identity. A block with `enabled: true` opts the language in and
    // declares the directive vocabulary. Consumed by the single language-
    // agnostic preprocessor pass -- NO engine code branches on the language
    // name.
    PreprocessConfig                                  preprocess;

    // Pratt-walker wrapper rule ids per `expr` shape (08.55 cleanup;
    // schema v4 `expr.wrapperRules`). Keyed by the expr rule's RuleId
    // value. The loader populates this BEFORE shape compile so the
    // shape-existence skip-list and the Pratt walker both read from
    // the same authoritative table. Empty for languages that declare
    // no `expr` shapes.
    std::unordered_map<std::uint32_t, ExprWrapperRules> exprWrapperRules;

    // Set of every wrapper RuleId synthesized by the Pratt walker
    // (union across `exprWrapperRules`). The shape-existence
    // validator (`validateOperatorBodyRules`) uses this to skip
    // interned rule names that have no compiled body BY DESIGN —
    // walker-managed frames, not user-declared shapes.
    std::unordered_set<std::uint32_t>                 wrapperRuleIds;

    // ── cross-language reference provenance (plan 29 P1+P2) ──────────────
    // Shape name → the label of the REFERENCED document that declared it,
    // for shapes folded in through `languageReferences`. Populated ONLY for
    // foreign shapes: a name that is absent from this map was declared by
    // the host document itself, which keeps the map empty for every
    // self-contained language and makes "is this shape foreign?" a single
    // lookup rather than a set difference.
    //
    // WHY IT IS LOAD-TIME DATA THAT NEVERTHELESS LIVES HERE: once two
    // documents contribute shapes, the JSON pointer `/shapes/<name>` that
    // every grammar diagnostic quotes is AMBIGUOUS — it names a location in
    // "the config" without saying which file to open. The six diagnostic
    // sites that format that pointer (`buildPositionTables`,
    // `detectAmbiguousAlternatives`, `validateBodyDefaultKindsOffGrammar`,
    // the reference-resolution pass, the wrapper-rule collision check and
    // `validateTypeNameCommitGuards`) all reach the schema data and nothing
    // else, so this is the one channel that reaches them without changing
    // six signatures. `shapePointer()` in the loader is the sole reader.
    std::unordered_map<std::string, std::string>      shapeOriginDoc;

    // Numeric-literal lexical grammar (08.55 cleanup; schema v4
    // `numberStyle`). nullopt for languages that declare no numeric
    // literals; required (loader emits `C_MissingNumberStyle`)
    // when the language declares `IntLiteral`/`FloatLiteral` tokens.
    std::optional<NumberStyle>                        numberStyle;

    // Per-language identifier character class (schema v4 `identifierClass`).
    // Empty `extraContinue` — the default for every language that declares no
    // block — means the universal ASCII+UTF-8 rule and nothing more.
    IdentifierClass                                   identifierClass;

    // Per-language semantic config (plan 08.6; schema v4 `semantics`
    // block). Empty / default-constructed when the language omits the
    // block — the analyzer then performs no semantic analysis for that
    // language. Read-only after construction; the loader is the only
    // writer.
    SemanticConfig                                    semantics;

    // Per-language CST→HIR lowering config (plan 09 HR8; schema v4
    // `hirLowering` block). Default-constructed (empty) when the language
    // omits the block — the lowering engine then produces nothing for it.
    // Read-only after construction; the loader is the only writer.
    HirLoweringConfig                                 hirLowering;

    // Per-RULE pipeline entry tier (plan 29; schema v4 `pipelineEntry`).
    // Empty when the language declares no block — every construct then takes
    // the ordinary CST→HIR→MIR→LIR path. Read-only after construction; the
    // loader is the only writer. See `pipeline_entry_config.hpp` for why the
    // declaration is per-CONSTRUCT and never per-language.
    PipelineEntryConfig                               pipelineEntry;

    // The text→LIR contract of an ASSEMBLY DIALECT (schema v4 `assembly`;
    // plan 29 P3/P4). `declared == false` for every language that is not one —
    // which is every shipped language except the `asm-*` dialect documents. The
    // loader is the only writer; see `assembly_config.hpp` for why a dialect is
    // a LANGUAGE and not a `.target.json` facet.
    AssemblyConfig                                    assembly;

    // ★★★ THE NON-FATAL DIAGNOSTICS THIS DOCUMENT PRODUCED WHILE LOADING
    // SUCCESSFULLY — D-CONFIG-WARNINGS-DISCARDED-ON-SUCCESSFUL-LOAD.
    //
    // `LoadResult<T> = std::expected<T, std::vector<ConfigDiagnostic>>` has an
    // ERROR side and no success side, so before this slot existed every
    // Warning/Info/Hint a clean load emitted was collected, counted by nothing,
    // and destroyed with the collector. That is 13 emit sites in
    // `grammar_schema_json.cpp` whose output no user has ever seen.
    //
    // ★★ WHY THE FACT LIVES ON THE OBJECT RATHER THAN BEING HANDED TO THE
    // CALLER AT THE LOAD SITE, which was the other candidate: two consumers
    // MEMOIZE the loaded schema (`program.cpp`'s per-target grammar cache and
    // `lsp/schema_cache.cpp`'s `byName_`), so a value returned only from the
    // load call is available only on a cache MISS — the warnings would appear
    // or not depending on which target happened to be compiled first. Hung off
    // the schema, the diagnostics travel with the cached object and any holder
    // can ask.
    //
    // ⚠ SEVERITY IS PRESERVED VERBATIM AND NOT FILTERED HERE. The loader only
    // reaches this slot when `hasErrors()` is false, so in practice it carries
    // no Errors — but filtering by severity at the producer would make this a
    // second, silently diverging definition of "what counts as fatal", which
    // `DiagnosticCollector::hasErrors()` already owns.
    std::vector<ConfigDiagnostic>                     loadDiagnostics;

    // ── Documents OTHER than this one that were folded into this build ──
    // Today that is every `languageReferences` target: the loader resolves
    // `<ref>.lang.json`, reads it, and merges its shapes, so the resulting
    // schema is a function of BOTH documents' bytes while `contentDigest()`
    // covers only the host's. Recording the resolved path and the referenced
    // bytes' digest is what lets a content-addressed consumer
    // (`ConfigDocumentMemo`) tell "the same host document" from "the same host
    // document over a DIFFERENT fragment" — a distinction the host digest
    // cannot make and whose failure mode is a silently wrong grammar.
    // EMPTY for a document that folds nothing in, and for the direct
    // `GrammarSchemaData` construction route that never touched a file.
    std::vector<ConfigDocumentDependency>             referencedDocuments;
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
    // Lowercase 64-hex SHA-256 of the EXACT document bytes this schema was
    // loaded from — the cache key for the runtime-object cache, which keys on
    // the config documents a build actually loaded.
    //
    // WHY RETAINED RATHER THAN RECOMPUTED. Re-walking `src/dss-config/` from
    // disk to hash it costs ~165 ms per invocation (MEASURED 2026-08-17: 86
    // files, 2,078,133 bytes; I/O-dominated — walk+read 152–160 ms, hash only
    // 9–13 ms), and would be paid on EVERY build. The loaders already hold the
    // bytes; they read them, parse them, and discard them. Digesting them
    // where they already are costs zero extra I/O and happens once per load
    // (loads are memoized in-process), and retaining 32 bytes of digest
    // instead of up to 440 KB of document is what makes retention free.
    //
    // ⚠ EMPTY MEANS UNKNOWN, NEVER "no content". A schema built through a path
    // that does NOT go via `loadFromText` — the public `GrammarSchemaData`
    // constructor above, which tests use to bypass JSON — has no document
    // bytes to digest and leaves this EMPTY. That is deliberate: an empty
    // digest is a DETECTABLE unknown a cache can refuse to key on, whereas a
    // fabricated or stale one is a silent wrong key. Every file route
    // (`loadShipped` → `loadFromFile` → `loadFromText`) is digested.
    [[nodiscard]] std::string_view             contentDigest()  const noexcept { return contentDigest_; }

    // ★★★ THE NAME THE CONFIG TREE IS INDEXED BY — the `.lang.json` stem —
    // WHICH IS NOT `name()`. `name()` is what the document DECLARES; only this
    // one is a valid `--language` argument or a valid path component.
    //
    // ⚠⚠ THE TWO DO NOT MERELY DIFFER IN CASE, AND READING THE DEFECT AS A
    // CASE PROBLEM UNDERSTATES IT BY HALF. ✔MEASURED over the shipped corpus
    // 2026-08-25 (`ShippedCorpusStemsAndDeclaredNames*` in
    // `tests/core/test_grammar_schema.cpp`, which re-measures it on every run):
    // of the SIX shipped language documents, five load as a root and are
    // nameable — the sixth, `asm.lang.json`, is the shared assembly line
    // grammar and is reachable only through another document's
    // `languageReferences`. Of the five nameable ones:
    //   * TWO differ only in case (`c` → "C", `toy` → "Toy");
    //   * THREE differ STRUCTURALLY — the stem's hyphens are gone from the
    //     declared name (`asm-arm64-gas` → "AsmArm64Gas", `asm-x86_64-att` →
    //     "AsmX86_64Att", `tsql-subset` → "TsqlSubset");
    //   * NONE agrees exactly.
    // For those three, `name()` used where a path or a `--language` argument
    // was meant fails on EVERY host — there is no `sources/AsmArm64Gas.lang.json`
    // on NTFS either. The class is NOT Linux-only.
    //
    // ⚠ WHAT *IS* HOST-DEPENDENT is the CASE-ONLY pair, and that is the half
    // that shipped: NTFS and APFS resolve `sources/C.lang.json` to the same
    // file, ext4 resolves it to nothing. ✔MEASURED 2026-08-25 — a caller that
    // used `name()` to build a `--language` argument gated 1656/1656 green on
    // Windows and took 527 tests down on the WSL leg. So a green Windows gate
    // is evidence about the case-only pair and about nothing else, and this
    // accessor exists so the correct name is the one that is easy to reach.
    //
    // ⓘ EMPTY for a grammar built from text with no document behind it
    // (`loadFromText(..., "<inline>")`). A caller that needs a path must not
    // fall back to `name()`, which is the very substitution this accessor
    // exists to stop — call `configDocumentPath()` below, which answers for
    // both cases and cannot be made to name a document that does not exist.
    [[nodiscard]] std::string_view             configName()     const noexcept { return configName_; }

    // ★★ THE DOCUMENT'S CONFIG-ROOT-RELATIVE PATH — `sources/<stem>.lang.json`,
    // composed HERE because this is where the stem, the subdirectory and the
    // suffix already live (`loadShipped` walks for exactly this shape). The
    // driver used to compose it at each use; two copies of a composition are
    // free to drift, and a drifted one is a runtime-cache key naming a document
    // nobody loaded.
    //
    // ⓘ TOTAL — there is no empty return and no failure arm to forget. A schema
    // with no document answers with a SELF-DESCRIBING NON-PATH
    // (`<inline language: NAME>`) rather than `sources/.lang.json`: the term is
    // a cache-key LABEL that sits beside the document's DIGEST, and naming a
    // plausible-looking file that was never read is how an auditor reading a key
    // document is misled. The digest is what identifies the input, and
    // `computeRuntimeObjectKey` already refuses an empty one outright.
    [[nodiscard]] std::string                  configDocumentPath() const;

    // ★★ WHERE THIS DOCUMENT CAME FROM — the label `loadFromText` was given,
    // which for every shipped and every file load is the ABSOLUTE path the
    // precedence walk resolved. EMPTY only for a text load with no document
    // behind it (`<inline>` and friends are stored verbatim, so a caller sees
    // the label rather than a lie).
    //
    // ★★★ IT EXISTS FOR ATTRIBUTION, AND ATTRIBUTION IS WHY IT IS PER-DOCUMENT
    // RATHER THAN PER-PROCESS —
    // [[D-PROGRAM-CONFIG-DIR-WALK-RESOLVES-A-FOREIGN-TREE]].
    // A diagnostic that refuses a spelling is making a claim about ONE
    // config document's contents, and the only honest way to name the tree that
    // claim came from is to ask the document. Asking the resolver again would
    // answer a DIFFERENT question — "which tree would a fresh walk find NOW" —
    // and the two really do diverge: `tests/core/test_config_path_walk.cpp`
    // mutates `DSS_CONFIG_ROOT` in-process at ten sites, and the examples runner
    // drives the compiler in-process, so a process-wide "last resolved root"
    // would be INVOCATION-ORDER DEPENDENT. This field is written once, at the
    // load, and is immutable like the rest of the schema.
    //
    // ⚠ NOT A CACHE-KEY TERM AND NOT A SUBSTITUTE FOR `configDocumentPath()`.
    // The key line wants the CONFIG-ROOT-RELATIVE spelling, which is stable
    // across machines; this is the absolute one, which is not. It belongs in
    // PROSE — a diagnostic, a report line — and nowhere a value is hashed or
    // compared.
    [[nodiscard]] std::string_view configDocumentOrigin() const noexcept {
        return documentOrigin_;
    }

    // Every OTHER document folded into this schema's build, with the digest
    // its bytes had at that moment — see `GrammarSchemaData::referencedDocuments`
    // for why `contentDigest()` alone cannot identify this schema's inputs.
    [[nodiscard]] std::span<detail::ConfigDocumentDependency const>
    referencedDocuments() const noexcept { return d_.referencedDocuments; }
    [[nodiscard]] std::string_view             name()           const noexcept { return d_.name; }
    [[nodiscard]] std::string_view             version()        const noexcept { return d_.version; }
    [[nodiscard]] std::uint32_t                schemaVersion()  const noexcept { return d_.schemaVersion; }
    [[nodiscard]] RuleInterner const&          rules()          const noexcept { return *d_.rules; }
    [[nodiscard]] SchemaTokenInterner const&   schemaTokens()   const noexcept { return *d_.schemaTokens; }
    [[nodiscard]] std::span<std::string const> fileExtensions() const noexcept { return d_.fileExtensions; }

    // ── Token recognition ──
    [[nodiscard]] std::span<LexemeMeaning const> lookupLexeme(std::string_view lexeme) const noexcept;
    [[nodiscard]] bool isEmptySpace(SchemaTokenId id) const noexcept;

    // Did the language's `tokens` map say ANYTHING about this token kind?
    // Distinguishes "declared, and deliberately not trivia" from "a built-in
    // kind this language never mentioned" — see `declaredLexemeTokens`.
    [[nodiscard]] bool declaresLexemeToken(SchemaTokenId id) const noexcept;

    // Longest declared lexeme key in bytes — used by the tokenizer to
    // bound its longest-match probe length so 5+ char lexemes can't
    // silently truncate. Computed at load time; zero only for an
    // empty lexemeTable.
    [[nodiscard]] std::size_t maxLexemeLength() const noexcept { return d_.maxLexemeLength; }

    // ── Longest-match probe planning ──
    //
    // The LENGTHS, DESCENDING, at which some declared key starts with
    // `lead` — the exact set of substring lengths a longest-match scan at a
    // position beginning with that byte can possibly match. Empty when no
    // declared key starts with `lead`, in which case the scan has no work to
    // do at all. See `detail::LexemeLengthIndex` for why this is exact
    // rather than an approximation, and why it is a derived index rather
    // than a cache.
    //
    // ⚠ A CALLER MUST STILL CALL `lookupLexeme` FOR EACH LENGTH IT KEEPS.
    // This answers "which lengths are worth asking about", never "does this
    // lexeme exist" — a length in the row means SOME key of that length
    // starts with that byte, not that THIS substring is one of them.
    [[nodiscard]] std::span<std::uint32_t const>
    lexemeLengthsForLeadByte(unsigned char lead) const noexcept {
        return lexemeLengths_.lengthsFor(lead);
    }

    // Same, for a lexer mode's `tokens` override table. Empty for every byte
    // when the mode declares no override (its `lookupLexemeInMode` probes all
    // miss, so skipping them is behaviour-preserving). Aborts on an invalid
    // mode id — the same strong-id contract `lookupLexemeInMode` holds, so a
    // caller can never confuse "wrong id" with "nothing declared".
    [[nodiscard]] std::span<std::uint32_t const>
    lexemeLengthsForLeadByteInMode(LexerModeId mode, unsigned char lead) const noexcept;

    // The number of table lookups a longest-match scan performs summed over
    // all 256 possible lead bytes, for the global table. Before the index
    // this was unconditionally `256 * maxLexemeLength`. Exposed so the
    // algorithmic property can be pinned by a test with no wall clock in it.
    [[nodiscard]] std::size_t lexemeProbeCount() const noexcept {
        return lexemeLengths_.probeCount();
    }

    // Config-driven parser expression-nesting cap (`parser.maxExpressionDepth`).
    // `nullopt` when the config omits the field — the CU build then keeps the
    // `ParserConfig` C++ fallback default. When present, a loader-validated
    // positive value the CU build copies onto `ParserConfig::maxExpressionDepth`
    // so the cap is config-driven, not a hardcoded engine constant. Names no
    // language — every shipped grammar reads its own value (or the fallback).
    [[nodiscard]] std::optional<std::size_t> maxExpressionDepth() const noexcept {
        return d_.maxExpressionDepth;
    }

    // Config-driven parser speculation-nesting cap (`parser.maxSpeculationDepth`).
    // `nullopt` when the config omits the field — the CU build then keeps the
    // `ParserConfig` C++ fallback default. When present, a loader-validated
    // positive value the CU build copies onto `ParserConfig::maxSpeculationDepth`,
    // from which the parser also derives the builder's checkpoint cap — so BOTH
    // stacked caps come from this ONE key and neither can bind invisibly behind
    // the other. Names no language — every shipped grammar reads its own value
    // (or the fallback).
    [[nodiscard]] std::optional<std::size_t> maxSpeculationDepth() const noexcept {
        return d_.maxSpeculationDepth;
    }

    // Config-driven speculative token-budget multiplier
    // (`parser.speculationBudgetFactor`). `nullopt` when the config omits the
    // field — the CU build then keeps the `ParserConfig` C++ fallback (16, the
    // value this was hardcoded to before it became a key, so an omitting
    // language is bit-identical to before). Names no language.
    [[nodiscard]] std::optional<std::size_t>
    speculationBudgetFactor() const noexcept {
        return d_.speculationBudgetFactor;
    }

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

    // Off-grammar body-token kinds. Every
    // `lexerModes.<name>.defaultToken.kind` declared in the schema
    // (`StringChar`, `BracketIdChar`, `CommentChar`, …) is a body
    // token: emitted by the tokenizer as a leaf while a body mode is
    // active, never referenced by any shape (loader-enforced via
    // `C_BodyDefaultKindInShape`). Both `TreeBuilder::pushToken` and
    // the parser dispatch loop consult this to skip schema-walker
    // advance for body tokens. Single source of truth, computed once
    // at schema-build time.
    //
    // `isBodyDefaultKind` is the preferred predicate — it hides the
    // container choice. `bodyDefaultTokenKinds` returns the set
    // directly for the hot-path consumers (parser + builder) that
    // already cache a pointer to it and would otherwise pay an
    // accessor call per token.
    [[nodiscard]] bool isBodyDefaultKind(SchemaTokenId id) const noexcept {
        return d_.bodyDefaultTokenKinds.contains(id);
    }
    [[nodiscard]] std::unordered_set<SchemaTokenId> const&
        bodyDefaultTokenKinds() const noexcept { return d_.bodyDefaultTokenKinds; }

    // FF11: true iff `id` is a token kind the tokenizer may pre-resolve
    // from a NON-MAIN mode context without a matching GLOBAL per-lexeme
    // entry — a per-mode `tokens` override kind, or a coalesced
    // `defaultToken` kind. See `Data::modeIntroducedKinds`. The builder's
    // synthetic-meaning drift guard accepts these (alongside body-default
    // and built-in-literal kinds) so a per-mode override token (e.g.
    // `HeaderStart`) is a clean leaf, not a fatal drift.
    [[nodiscard]] bool isModeIntroducedKind(SchemaTokenId id) const noexcept {
        return d_.modeIntroducedKinds.contains(id);
    }

    // Per-`SchemaTokenId` flags channel used by tokenizer emit sites
    // that don't pass through the lexeme-meaning lookup (e.g. numeric
    // literals, where the tokenizer hand-codes the scan and the kind
    // is the built-in `IntLiteral`/`FloatLiteral` rather than a
    // schema-declared lexeme). Today every kind returns
    // `NodeFlags::None`; this is the structural channel a future
    // schema field (e.g. `literalFlags: { IntLiteral: [...] }`) would
    // populate. The accessor exists so the numeric emit site uses the
    // same `flagsApplied`-aware shape as the other emit sites and
    // doesn't drift if a use case lands.
    [[nodiscard]] NodeFlags flagsForKind(SchemaTokenId id) const noexcept;

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

    // Enumerate the RuleLeaf branch rules reachable from `cur` through
    // AltChoice positions, in DECLARED grammar order — the JSON-array
    // order the grammar author wrote, depth-first through nested
    // AltChoice positions (exactly the order `routeToRuleLeaf` tries
    // branches; first occurrence wins for a rule reachable through
    // more than one branch). Token-leaf branches are NOT reported —
    // token routing goes through `advance`. Returns `{rule}` when
    // `cur` is already at RuleLeaf(rule); empty for an invalid cursor
    // or when no RuleLeaf is reachable.
    //
    // Consumed by the parser's AltChoice candidate enumeration so that
    // branch probe order is author-controlled (declared order), never
    // an accident of rule-interner id (= alphabetical name) order.
    //
    // c97: the enumeration is PRECOMPUTED at schema construction (the DFS
    // depends only on static grammar data), so this is now a span read —
    // O(1), no per-call DFS or allocation. The span is stable for the
    // schema's lifetime.
    [[nodiscard]] std::span<RuleId const>
    altRuleBranches(SchemaCursor cur) const noexcept;

    [[nodiscard]] std::span<SchemaTokenId const> expectedSet(SchemaCursor cur) const noexcept;

    // c97 O(1) membership queries (bitset-backed; built once at schema
    // construction from the same loader data the span accessors expose).
    // Each is exactly equivalent to a linear find over the corresponding
    // span — the parser's per-token gates call these instead.
    //   * expectedSetContains(cur, tok)  ≡ find(expectedSet(cur), tok)
    //   * firstSetContains(rule, tok)    ≡ find(firstSetOf(rule), tok)
    //   * predictivePrefixExcludes(rule, i, tok) ≡ the predictive-prune
    //     test at offset i: TRUE iff the offset is defined, carries a
    //     non-empty admissible set, and `tok` is NOT in it (an empty /
    //     undefined offset means "no constraint" → never excludes).
    [[nodiscard]] bool expectedSetContains(SchemaCursor cur,
                                           SchemaTokenId tok) const noexcept;
    [[nodiscard]] bool firstSetContains(RuleId rule,
                                        SchemaTokenId tok) const noexcept;
    [[nodiscard]] bool predictivePrefixExcludes(RuleId rule, std::size_t offset,
                                                SchemaTokenId tok) const noexcept;

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

    // Nullable-tail introspection used by the parser to detect and
    // step past skippable `optional`/`repeat` shapes.
    //
    // `nullableTail(cur)`: true when the position can complete to
    // end-of-rule without consuming a token.
    //
    // `nullableBranch(cur)`: at an AltChoice cursor, returns the
    // first branch whose `nullableTail` is true; invalid otherwise.
    // "First wins" is deliberate: the only AltChoice shapes the
    // loader produces with multiple nullable branches are
    // `optional` (two branches, second is the skip) and `repeat`
    // (two branches, second is the loop exit) — both unambiguous
    // by construction. A hand-rolled `alt` with multiple nullable
    // arms is loader-rejected via `C_AmbiguousAlternatives` on
    // overlapping FIRST sets, which empirically covers every
    // multi-nullable case the loader can emit.
    [[nodiscard]] bool         nullableTail(SchemaCursor cur)   const noexcept;
    [[nodiscard]] SchemaCursor nullableBranch(SchemaCursor cur) const noexcept;

    // Direct per-rule queries that don't require a cursor instance.
    [[nodiscard]] std::span<SchemaTokenId const> firstSetOf(RuleId rule) const noexcept;
    [[nodiscard]] std::span<SchemaTokenId const> followSetOf(RuleId rule) const noexcept;
    [[nodiscard]] bool                           isNullable(RuleId rule) const noexcept;

    // LL(k) PREDICTIVE PREFIX of `rule` — the per-offset over-approximation
    // of the token sequences that can begin a derivation of `rule`, used by
    // the parser to PRUNE speculative-alt candidates without backtracking.
    // `predictivePrefixOf(rule)[i]` is the (sorted) exact set of token kinds
    // admissible as the (i)-th consumed token across the rule's leading run
    // of single-token elements (see `CompiledRule::predictivePrefix`).
    //
    // Empty when the rule carries no multi-token discriminator (entry is a
    // lone non-terminal / AltChoice, or the rule is nullable/empty) — the
    // caller then falls back to the standard 1-token FIRST gate. Each inner
    // span is stable for the schema's lifetime. Config-derived; names no
    // token, rule, or language.
    [[nodiscard]] std::size_t predictivePrefixLen(RuleId rule) const noexcept;
    [[nodiscard]] std::span<SchemaTokenId const>
    predictivePrefixAt(RuleId rule, std::size_t offset) const noexcept;

    // D-PARSE-PREDICTIVE-PRUNE-CONTEXTUAL-KEYWORD: true iff `kind` is a
    // contextual / scope-resolvable token kind (a soft keyword the builder may
    // demote to Identifier). The LL(k) predictive prune skips any offset whose
    // observed token is contextual, so it never prunes a candidate the demoted
    // token would match. Config-derived (the `contextual` LexemeMeaning flag /
    // `reservedWordPolicy`); names no token, rule, or language.
    [[nodiscard]] bool isContextualKind(SchemaTokenId kind) const noexcept;

    // Schema-declared panic-mode sync tokens. Sorted ascending by
    // `id.v`. Empty when the config omits the `syncTokens` field.
    // Parser's panic-mode recovery consumes until peek is in this set
    // OR in `followSetOf(currentRule)`.
    [[nodiscard]] std::span<SchemaTokenId const> syncTokens() const noexcept;

    // Per-language type-extension declarations (SP2; schema v3 `typeExtensions[]`).
    // Empty for v1/v2 configs. Consumed by registerSchemaTypeExtensions.
    [[nodiscard]] std::span<TypeExtensionDescriptor const> typeExtensions() const noexcept;

    // Artifact profiles this language supports (plan 06 AP1; schema v4
    // optional `artifactProfiles[]`). Empty when the field is absent. Each
    // entry is a loader-validated registered profile name. Consumed by the
    // driver (AP2+) to reject a project requesting an unsupported profile.
    [[nodiscard]] std::span<std::string const> artifactProfiles() const noexcept;

    // The instruction-set architecture this language EMITS (optional top-level
    // `isa`). EMPTY ⇒ the language is PORTABLE and builds for every target —
    // that is the default and the overwhelmingly common case. Consumed by
    // `crossValidateLanguageTarget` (program/) against `TargetSchema::isa()`.
    // See `GrammarSchemaData::isa` for why an empty value is portability and
    // not "unknown", and for why this is not a capability enumeration.
    [[nodiscard]] std::string_view isa() const noexcept;

    // Config-driven import resolution (schema v4 `imports` block). Default
    // `ImportStrategy::None` when the config omits the block. Consumed by
    // chooseResolver/ConfigDrivenImportResolver — the single language-agnostic
    // import engine; NO engine code branches on the language name.
    [[nodiscard]] ImportConfig const& imports() const noexcept;

    // Config-driven C-preprocessor (schema v4 `preprocess` block). Default
    // `enabled == false` when the config omits the block. Consumed by the
    // single language-agnostic preprocessor pass; NO engine code branches on
    // the language name.
    [[nodiscard]] PreprocessConfig const& preprocess() const noexcept;

    // `expr`-shape introspection. `isExprRule` is true when the rule's
    // body was declared as `{ "expr": { "atom": ..., "minPrecedence": ... } }`.
    // For such rules `exprAtom` returns the operand rule and
    // `exprMinPrecedence` returns the floor precedence for the outermost
    // operator climb. Non-expr rules return false / InvalidRule / 0.
    [[nodiscard]] bool         isExprRule(RuleId rule)        const noexcept;
    [[nodiscard]] RuleId       exprAtom(RuleId rule)          const noexcept;
    [[nodiscard]] std::int32_t exprMinPrecedence(RuleId rule) const noexcept;

    // Type-name commit guard (`commitRequiresTypeName` on the shape body;
    // FC2 cast-expression disambiguation). Returns the RuleId of the
    // declared type-position child rule, or InvalidRule when the shape
    // declares no guard (every rule before FC2). When valid, a
    // structurally-successful speculative probe of `rule` must pass the
    // parser's generic type-name triage before committing — see
    // `Parser::Impl::typeNameCommitApproved_`.
    [[nodiscard]] RuleId       typeNameCommitRule(RuleId rule) const noexcept;
    // FC4 c1 (M4): the guard's UNKNOWN-name polarity (the bare-string
    // config form keeps `PreferType`; the object form may select
    // `RequireKnownType` — C 6.7.6.3p11). Only meaningful when
    // `typeNameCommitRule(rule).valid()`; returns the default otherwise.
    [[nodiscard]] TypeNameCommitPolarity
        typeNameCommitPolarity(RuleId rule) const noexcept;

    // commitAfterPrefix CUT (`commitAfterPrefix` on the shape body; PEG
    // "cut"; D-CSUBSET-LABEL-BUDGET-CLIFF, p19 Cluster G c31). True iff a
    // speculative probe of `rule` should COMMIT the moment the rule's fixed
    // leading token-prefix (`predictivePrefixLen(rule)` tokens) is consumed
    // without failure — the rest of the rule then parses non-speculatively
    // (no probe budget). False for every rule that omits the flag. The cut
    // is sound only where no other alternative can match past that prefix;
    // the config author asserts that by setting the flag. See
    // `Parser::Impl::trySpeculativeBranch`. Names no token, rule, or
    // language.
    [[nodiscard]] bool commitAfterPrefix(RuleId rule) const noexcept;

    // Pratt-walker wrapper rule ids declared by `expr.wrapperRules`
    // for `rule`. The loader auto-interned the declared names and
    // validated all three were present, so for an `isExprRule(rule)`
    // the returned struct is `.valid()`. For non-expr rules every
    // field is `InvalidRule`. Read once per `walkExpression` entry —
    // the walker bundles the three ids into its `PrattRules` and
    // threads them through the climb.
    [[nodiscard]] ExprWrapperRules exprWrapperRules(RuleId rule) const noexcept;

    // True iff `rule` is one of the Pratt-walker auto-interned wrapper
    // rules (binary / unary / postfix / ternary). The loader populates
    // the union-set at compile time; this is O(1) hash-set membership.
    //
    // **Why the SchemaWalker needs this**: auto-interned wrappers are
    // entered via `wrapLastChildExprFrame` for structural reparenting
    // of an already-built operand, NOT via the schema's standard
    // routing dispatch (the wrapper rules have no body in the position
    // graph — they exist only for tree shape). Cursor advances inside
    // a wrap frame would trip the `cursorDesynced_` latch as a false
    // positive — the latch's contract is "real grammar mismatch," not
    // "Pratt wrapper structural-only frame." The walker consults this
    // accessor to suppress the latch while the current frame is a
    // wrap, fixing the speculative-alt + postfix-wrap interaction
    // (plan 05 post-close sub-cycle B).
    [[nodiscard]] bool isAutoInternedWrapperRule(RuleId rule) const noexcept;

    // Numeric-literal lexical grammar declared by the language's
    // `numberStyle` block. Returns nullptr when no block was
    // declared — the tokenizer then knows the language has no
    // numeric literals (e.g. toy). The loader rejects schemas that
    // declare `IntLiteral`/`FloatLiteral` tokens without a block
    // (`C_MissingNumberStyle`), so any reachable scanNumber call
    // sees a non-null pointer.
    [[nodiscard]] NumberStyle const* numberStyle() const noexcept;

    // Per-language identifier character class (schema v4 `identifierClass`).
    // ⚠ RETURNED BY REFERENCE TO A DEFAULTED STRUCT, NOT AS A NULLABLE POINTER
    // LIKE `numberStyle`: the tokenizer needs an answer for EVERY byte of every
    // identifier run, so "the language declared nothing" and "the language
    // declared the universal rule" must be the same object rather than a null
    // the hot path has to branch on.
    [[nodiscard]] IdentifierClass const& identifierClass() const noexcept;

    // Per-language semantic config (plan 08.6; schema v4 `semantics`).
    // Default-constructed (every facet empty) when the language omits
    // the block — the analyzer then performs zero semantic analysis
    // for that language and the model produces no symbols/types/
    // diagnostics. Read-only; the loader is the only writer.
    [[nodiscard]] SemanticConfig const& semantics() const noexcept;

    // Per-language CST→HIR lowering config (plan 09 HR8; schema v4
    // `hirLowering`). Default-constructed (`empty()`) when the language omits
    // the block. Read-only; the loader is the only writer.
    [[nodiscard]] HirLoweringConfig const& hirLowering() const noexcept;

    // Per-RULE pipeline entry tier (plan 29; schema v4 `pipelineEntry`).
    // Empty when the language declares no block. The engine that consumes it
    // dispatches on `PipelineTier` — a closed enum — and must NEVER read the
    // language name; a tier whose entry path this build has not implemented
    // is refused AT USE with a precise diagnostic, never demoted to another
    // tier. Read-only; the loader is the only writer.
    [[nodiscard]] PipelineEntryConfig const& pipelineEntry() const noexcept;

    // The assembly-dialect text→LIR contract (plan 29; schema v4 `assembly`).
    // `declared == false` for every non-dialect language, which is how a caller
    // distinguishes "this language has no assembly surface" from "this dialect
    // declared an empty instruction table". Read-only; the loader is the only
    // writer.
    [[nodiscard]] AssemblyConfig const& assembly() const noexcept;

    // The non-fatal diagnostics this document emitted while loading
    // SUCCESSFULLY (D-CONFIG-WARNINGS-DISCARDED-ON-SUCCESSFUL-LOAD). Empty for
    // a clean document. Feed it to `forwardConfigDiagnostics` to surface it on
    // a DiagnosticReporter — that helper already preserves severity verbatim,
    // so a warning stays a warning and `--warnings-as-errors` still governs.
    // ⚠ A FAILED load returns its diagnostics through `LoadResult`'s error side
    // instead; there is no GrammarSchema object to ask in that case, which is
    // why the two channels are not merged into one.
    [[nodiscard]] std::span<ConfigDiagnostic const> loadDiagnostics() const noexcept;

    // ── Scope rules ──
    [[nodiscard]] bool isTokenValidInScope(SchemaTokenId tok,
                                           std::span<ScopeKind const> stack) const noexcept;

    // ── Termination ──
    [[nodiscard]] bool canEndSource(SchemaCursor cur) const noexcept;

private:
    // c97: dense per-rule compiled-shape table, indexed by RuleId.v — the
    // ctor's sealing pass drains the loader's `d_.compiledRules` map into
    // this vector (RuleIds are dense interner ids: 1..N). An index with no
    // loader entry (auto-interned Pratt wrapper rules; the 0 sentinel)
    // holds a default CompiledRule, whose `entryPos == 0` / empty members
    // reproduce the former map-miss behavior exactly. Every per-token
    // grammar query indexes this table — no hash, no probe.
    std::vector<detail::CompiledRule> compiledDense_;

    // Per-lead-byte declared-length indexes for the global lexeme table and
    // for each lexer mode's override table, derived by the ctor's sealing
    // pass from the very tables `lookupLexeme` / `lookupLexemeInMode` query.
    // ★ THE MODE INDEXES ARE PER MODE, NOT A GLOBAL UNION. A union would be
    // conservative-correct but would filter far less: the shipped `c`
    // grammar's `directive` mode overrides ONE key, `include`, so its own row
    // for `i` is a single length — while the GLOBAL row for `i` is four
    // (`include`, `inline`, `int`, `if`), three of them guaranteed misses in a
    // one-entry override table. `tests/core/test_grammar_schema.cpp`
    // (`GrammarSchemaProbeIndex.ModeRowsComeFromThatModesOwnTableNotAGlobalUnion`)
    // is what goes red if these are ever collapsed into one.
    // Indexed by `LexerModeId::v`; ids are dense 1..N with slot 0 the
    // InvalidLexerMode sentinel, so this parallels `d_.lexerModes`.
    detail::LexemeLengthIndex              lexemeLengths_;
    std::vector<detail::LexemeLengthIndex> modeLexemeLengths_;

    // O(1) presence-tolerant row access: nullptr only for out-of-range ids
    // (an in-range id with no compiled body returns the default row, whose
    // empty fields answer every query the way the old map-miss did).
    [[nodiscard]] detail::CompiledRule const* ruleRow(std::uint32_t v) const noexcept {
        return v < compiledDense_.size() ? &compiledDense_[v] : nullptr;
    }

    // The `.lang.json` stem this schema was loaded from — see `configName()`.
    // EMPTY when there was no document.
    std::string configName_;

    // The label this document was loaded UNDER — see `configDocumentOrigin()`.
    std::string documentOrigin_;

    // Lowercase 64-hex SHA-256 of the document bytes — see `contentDigest()`.
    // Written ONLY by `loadFromText` (a static member, so no friend is
    // needed); every other construction path leaves it empty on purpose.
    std::string contentDigest_;

    detail::GrammarSchemaData d_;
};

// Forward every ConfigDiagnostic from a failed `loadShipped` /
// `loadFromFile` / `loadFromText` into a DiagnosticReporter. Hoisted
// here (post-FF2 audit silent-failure C1) from program.cpp's anon
// namespace once a second consumer arrived (FF2 c_header_parser):
// without this, FF1/FF2 wrappers only emit their tier's wrap code
// (`F_HeaderGrammarLoadFailed` / `D_SchemaLoadFailed`) and the
// underlying C_* config diagnostics are silently dropped. Inline
// so no new TU is required.
inline void forwardConfigDiagnostics(std::span<ConfigDiagnostic const> diags,
                                      DiagnosticReporter& dst) {
    for (auto const& cd : diags) {
        ParseDiagnostic p;
        p.code     = cd.code;
        p.severity = cd.severity;
        // Prefix `path` with `at ` so downstream tooling can
        // distinguish a JSON pointer (`at /sections/0`) from a
        // free-form loader message (`: missing required field`).
        if (!cd.path.empty()) p.actual = "at " + cd.path;
        if (!cd.message.empty()) {
            if (!p.actual.empty()) p.actual += ": ";
            p.actual += cd.message;
        }
        dst.report(std::move(p));
    }
}

} // namespace dss
