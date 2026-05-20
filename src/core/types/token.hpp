#pragma once

#include "core/export.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>

namespace dss {

// Lexer-level token category. The tokenizer assigns this *before* any schema
// is consulted: it knows whether it saw an integer, a string, a punctuation
// run, etc. — but not whether `+` here means SumOperator or StringAppendOperator.
// That second decision is the schema's job (see SchemaTokenId + GrammarSchema).
enum class CoreTokenKind : std::uint16_t {
    Unknown = 0,

    Identifier,
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    CharLiteral,
    BoolLiteral,
    NullLiteral,

    Punctuation,    // {} () [] , ; etc. — exact lexeme via span
    Operator,       // + - * / == != etc. — exact lexeme via span
    Word,           // alphanumeric run; keyword-vs-identifier resolved later

    Whitespace,
    LineComment,
    BlockComment,
    Newline,

    Eof,
    Error,
};

// One token in the source stream.
//
// - coreKind   : assigned by the tokenizer (e.g. Word / Operator / IntLiteral).
// - schemaKind : assigned by the tokenizer via mode-aware lookup against
//                `GrammarSchema::lookupLexeme` / `lookupLexemeInMode`.
//                Picks the priority-winner candidate (lowest `priority` value;
//                declaration order on ties). Scope-stack filtering and
//                contextual-keyword demotion remain TreeBuilder::pushToken's
//                job — those need state the tokenizer does not track.
//                May be left InvalidSchemaToken in two cases:
//                  - hand-built tokens (tests that fabricate Tokens directly);
//                  - Word fallback (the lexeme isn't a keyword — builder
//                    promotes to Identifier).
//                The builder treats schemaKind as a hint: when valid AND
//                scope-allowed, trust it; otherwise re-resolve from the
//                lexeme via the full candidate-filter path.
// - span       : byte range in the source buffer. The lexeme text is recovered
//                via SourceBuffer::slice(span); never stored on the token itself.
struct DSS_EXPORT Token {
    CoreTokenKind coreKind   = CoreTokenKind::Unknown;
    SchemaTokenId schemaKind = InvalidSchemaToken;
    SourceSpan    span       = SourceSpan::empty(0);
};
static_assert(sizeof(Token) <= 16, "Token should stay small");

} // namespace dss
