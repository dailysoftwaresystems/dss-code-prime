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
// - coreKind   : assigned by the tokenizer.
// - schemaKind : assigned by the schema-aware resolver inside TreeBuilder::pushToken.
//                Invalid (SchemaTokenId{0}) until the builder resolves it.
// - span       : byte range in the source buffer. The lexeme text is recovered
//                via SourceBuffer::slice(span); never stored on the token itself.
struct DSS_EXPORT Token {
    CoreTokenKind coreKind   = CoreTokenKind::Unknown;
    SchemaTokenId schemaKind = InvalidSchemaToken;
    SourceSpan    span       = SourceSpan::empty(0);
};
static_assert(sizeof(Token) <= 16, "Token should stay small");

} // namespace dss
