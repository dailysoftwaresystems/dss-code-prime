#pragma once

#include "core/export.hpp"
#include "core/types/scope_kind.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

enum class DiagnosticSeverity : std::uint8_t {
    Hint,
    Info,
    Warning,
    Error,
};

[[nodiscard]] DSS_EXPORT std::string_view severityName(DiagnosticSeverity s) noexcept;

// Stable, exhaustively-switchable diagnostic identity. Strings derive from
// this in the formatter via diagnosticCodeText(). Prefixes group the
// originating phase:
//   P_*    parse-time / tree-builder
//   C_*    config-load (GrammarSchema)
//   S_*    semantic (later)
//   I_*    IR-gen   (later)
//
// Values are stable across versions — they appear in user-facing output
// and may be referenced from --suppress flags. Never renumber an existing
// entry; append new ones at the end of the relevant range.
enum class DiagnosticCode : std::uint16_t {
    None                          = 0,

    // ── P0xxx — parser / tree-builder ──
    P_UnexpectedToken             = 0x0001,
    P_MissingRequiredChild        = 0x0002,
    P_UnknownToken                = 0x0003,
    P_PrematureEndOfInput         = 0x0004,
    P_InvalidEscapeSequence       = 0x0005,
    P_NumericLiteralOutOfRange    = 0x0006,
    P_DeprecatedSyntax            = 0x0007,
    P_AmbiguousToken              = 0x0008,
    P_NoAlternativeMatched        = 0x0009,
    P_UnclosedScope               = 0x000A,
    P_UnmatchedClose              = 0x000B,
    P_ContextualKeywordResolution = 0x000C,
    P_SchemaCursorDesync          = 0x000D,
    P_IllegalChar                 = 0x000E,
    P_MalformedNumber             = 0x000F,
    P_UnterminatedString          = 0x0010,
    P_UnterminatedComment         = 0x0011,
    P_InvalidEscape               = 0x0012,

    // ── P9xxx — builder internal-invariant violations (release-mode rescues) ──
    P_BuilderInvariant            = 0x9000,
    P_TooManyDiagnostics          = 0x9001,
    P_UnfinishedTree              = 0x9002,
    P_RecoveryStalled             = 0x9003,
    P_MaxSpeculationDepth         = 0x9004,
    P_UncommittedCheckpoint       = 0x9005,
    P_BacktrackFailed             = 0x9006,

    // ── C0xxx — config loader (see plan §5.12) ──
    C_MissingField                = 0xC001,
    C_UnknownShape                = 0xC002,
    C_UnknownToken                = 0xC003,
    C_VersionMismatch             = 0xC005,
    C_CircularShape               = 0xC007,
    C_AmbiguousAlternatives       = 0xC010,
    C_UnclosableScope             = 0xC011,
    C_MalformedJson               = 0xC020,
    C_InvalidLanguageName         = 0xC021,
    C_InvalidPrecedenceTable      = 0xC022,
    C_RedundantScopeRequire       = 0xC023,
    C_ConflictingField            = 0xC024,
    C_UnknownScopeName            = 0xC025,
    C_RedundantField              = 0xC026,
    C_UnknownLexerMode            = 0xC027,
    C_InvalidStringStyle          = 0xC028,
    // A `lexerModes.<m>.defaultToken.kind` was also referenced from a
    // `shapes/*` rule. The builder treats body-default kinds as off-
    // grammar (cursor-skip), so a shape reference would silently never
    // match — surface it at load time instead.
    C_BodyDefaultKindInShape      = 0xC029,
    // Type-extension declarations (SP2; `typeExtensions[]`, schema v3).
    // C_UnknownTypeExtension: a malformed extension entry at load (not an
    //   object / not a valid declaration); ALSO the code a consumer (phase #8
    //   / transpile-map validation) emits when a type references an extension
    //   name that no registry resolved.
    // C_TypeExtensionParamMismatch: an extension parameter has an unknown kind
    //   (not "Integer"/"Type") or a malformed parameter spec.
    C_UnknownTypeExtension        = 0xC02A,
    C_TypeExtensionParamMismatch  = 0xC02B,
    // The `imports` block (schema v4) is malformed: `strategy` missing or not
    // one of "none"/"include-following"/"name-matching", or a required field
    // has the wrong JSON type. (Missing-but-required fields use C_MissingField;
    // unknown rule/token names use C_UnknownShape/C_UnknownToken.)
    C_InvalidImports              = 0xC02C,
    // An `expr` shape was declared without a complete `wrapperRules` block
    // (08.55 cleanup; schema v4). Every `expr`-shape rule must name the three
    // Pratt-walker wrapper rules (binary / unary / postfix); the engine
    // auto-interns and threads those names — it never hardcodes them.
    C_MissingWrapperRules         = 0xC02D,
    // The language declared `IntLiteral`/`FloatLiteral` as multi-char tokens
    // but no `numberStyle` block (08.55 cleanup; schema v4). The tokenizer's
    // scanNumber() drives entirely from the schema's NumberStyle; without it
    // the scanner has no rules to apply. Use ONLY for the "block is required
    // but absent" case at the end of the numberStyle parse — type/shape/
    // range errors inside an existing block use C_InvalidNumberStyle.
    C_MissingNumberStyle          = 0xC02E,
    // The `numberStyle` block is present but malformed: wrong JSON type,
    // out-of-range radix, non-single-char fractionPoint/digitSeparator,
    // unknown emitKind reference, etc. Mirrors the `imports` block's
    // C_InvalidImports discipline (08.55 cleanup; schema v4). Missing
    // required sub-fields use C_MissingField; unknown sub-keys use
    // C_UnknownShape.
    C_InvalidNumberStyle          = 0xC02F,
    // Two or more `wrapperRules` roles (binary/unary/postfix) resolved to the
    // same RuleId — a duplicate-name config error. Distinct from
    // `C_MissingWrapperRules` (missing field) so the operator sees the actual
    // class of failure: the three Pratt-walker frames MUST be distinct or the
    // walker's tree-building corrupts silently.
    C_DuplicateWrapperRules       = 0xC030,
    // The `semantics` block (schema v4) is present but malformed: wrong JSON
    // shape, unknown `kind`/`core`/`constructor`/`nameMatch` enum string, an
    // out-of-range child index, or a typeShape `operandChild` that doesn't
    // point to a valid visible child slot. (Missing required sub-fields stay
    // `C_MissingField`; dangling rule/token names stay `C_UnknownShape`/
    // `C_UnknownToken`.)
    C_InvalidSemantics            = 0xC031,

    // ── S0xxx — semantic analysis (phase #8; see 08.6-semantic-plan §3) ──
    // Emitted by the language-agnostic semantic analyzer
    // (`src/analysis/semantic/`). The 0xE high nibble renders as the letter
    // `S` (see `diagnosticCodePrefix`). Append, never renumber.
    S_UndeclaredIdentifier        = 0xE001,
    S_RedeclaredSymbol            = 0xE002,
    S_TypeMismatch                = 0xE003,
    S_NotCallable                 = 0xE004,
    S_ArgCountMismatch            = 0xE005,
    S_UnknownType                 = 0xE006,

    // ── D0xxx — driver / compilation-unit (see 08-compilation-unit-plan §2.6) ──
    // Emitted into a CompilationUnit's driver-level reporter by UnitBuilder.
    // The 0xD block is shared with future driver codes (e.g. the artifact-
    // profile plan's D_ArtifactProfileNotSupported); append, never renumber.
    D_FileNotFound                = 0xD001,
    D_EmptyInput                  = 0xD002,
    D_DuplicateFile               = 0xD003,
    // Import resolution (08-compilation-unit-plan §2.8, CU4). A reference to
    // another translation unit could not be resolved within the CU:
    //   D_UnresolvedImport    — a c-subset `#include "x.h"` whose file was not
    //                           found in the including dir or any include dir.
    //   D_UnresolvedReference — a tsql table reference (qualifiedName in table
    //                           position) with no matching CREATE TABLE in the CU.
    // Both are Warnings: phase #8 / FFI / a system catalog may still provide
    // the target, so the driver does not treat them as build-fatal here.
    D_UnresolvedImport            = 0xD004,
    D_UnresolvedReference         = 0xD005,
};

// Symbolic name like "P_UnexpectedToken" / "C_MalformedJson" / "P0042".
[[nodiscard]] DSS_EXPORT std::string_view diagnosticCodeName(DiagnosticCode c) noexcept;

// Formatted prefix like "P0001" / "C0010" — used by the renderer for the
// header line ("error[P0001]: ...").
[[nodiscard]] DSS_EXPORT std::string diagnosticCodePrefix(DiagnosticCode c);

// A secondary location attached to a diagnostic — "matching opener here",
// "previously declared here", etc. May reference a *different* buffer than
// the diagnostic's primary span (cross-file includes, when we get them).
struct DSS_EXPORT RelatedLocation {
    BufferId    buffer;
    SourceSpan  span;
    std::string note;
};

// One diagnostic record. Pure data — formatting lives in DiagnosticReporter.
struct DSS_EXPORT ParseDiagnostic {
    DiagnosticCode      code     = DiagnosticCode::None;
    DiagnosticSeverity  severity = DiagnosticSeverity::Error;

    BufferId    buffer;             // primary buffer
    SourceSpan  span = SourceSpan::empty(0);

    std::optional<RuleId> ruleContext;   // which expected shape was active

    // What the schema would have accepted at this position. Pre-rendered
    // strings — the builder populates these from `schemaTokens().name()` /
    // `rules().name()` at emit time, e.g. {"';'", "expression"}.
    std::vector<std::string> expected;

    // What was actually seen — lexeme text or token-kind name.
    std::string actual;

    // Builder-captured scope stack at the moment of error. Powers
    // "got '>' while inside Generic scope" without state reconstruction.
    std::vector<ScopeKind> scopeStack;

    // Secondary locations (matching opener, previous declaration, ...).
    std::vector<RelatedLocation> related;

    // Optional human-friendly hint ("did you forget a semicolon?").
    std::string suggestion;
};

} // namespace dss
