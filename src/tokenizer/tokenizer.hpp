#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "tokenizer/token_stream.hpp"

#include <memory>
#include <utility>

namespace dss {

// Schema-aware byte-stream tokenizer. Single-use:
//
//   Tokenizer tk{src, schema};
//   auto [stream, reporter] = std::move(tk).tokenize();
//
// Owns the LexerModeStack instance for its lifetime. TZ1 ships the
// no-modes path only (always operates in the schema's "main" mode);
// TZ2 wires real mode-stack management.
//
// Emits *every* token including whitespace, comments, and the trailing
// Eof. Schema `EmptySpace` flagging is applied via the meaning's
// `flagsApplied` set at builder time — the tokenizer does not drop or
// coalesce whitespace.
//
// Schema resolution: Token.schemaKind is filled by the tokenizer via
// `GrammarSchema::lookupLexeme` (TZ1) / `lookupLexemeInMode` (TZ2),
// picking the highest-priority candidate. Scope filtering and
// contextual-keyword demotion remain `TreeBuilder::pushToken`'s job —
// those need the schema cursor + scope stack, which the tokenizer
// doesn't track.
class DSS_EXPORT Tokenizer {
public:
    Tokenizer(std::shared_ptr<SourceBuffer>        src,
              std::shared_ptr<GrammarSchema const> schema,
              DiagnosticReporter::Config           diagConfig = {});

    Tokenizer(Tokenizer const&)            = delete;
    Tokenizer& operator=(Tokenizer const&) = delete;
    Tokenizer(Tokenizer&&)                 = delete;
    Tokenizer& operator=(Tokenizer&&)      = delete;

    // Consume the entire source buffer. Returns the stream + the
    // diagnostic reporter ownership. The reporter holds every diagnostic
    // emitted during tokenization (`P_IllegalChar`, future TZ2 codes
    // like `P_UnterminatedString`, etc.).
    [[nodiscard]] std::pair<TokenStream, std::unique_ptr<DiagnosticReporter>>
        tokenize() &&;

private:
    std::shared_ptr<SourceBuffer>        source_;
    std::shared_ptr<GrammarSchema const> schema_;
    std::unique_ptr<DiagnosticReporter>  reporter_;
};

} // namespace dss
