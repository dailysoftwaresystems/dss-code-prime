#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "tokenizer/token_stream.hpp"

#include <memory>

namespace dss {

// Return aggregate from `Tokenizer::tokenize()`. Named fields beat
// `std::pair` for self-documenting call sites and survive future
// additions (per-run stats, etc.) without rippling through every caller.
struct DSS_EXPORT TokenizeResult {
    TokenStream                         stream;
    std::unique_ptr<DiagnosticReporter> diagnostics;
};

// Schema-aware byte-stream tokenizer. Single-use:
//
//   Tokenizer tk{src, schema};
//   auto [stream, diagnostics] = std::move(tk).tokenize();
//
// Mode-aware: the operator/identifier scan consults
// `GrammarSchema::lookupLexemeInMode` for the active scan frame's mode
// with a global fallback (context-sensitive lexing); the active mode is
// carried per scan frame and switched by `modeOp` token side-effects.
//
// Emits *every* token including whitespace, comments, and the trailing
// Eof. Schema `EmptySpace` flagging is applied via the meaning's
// `flagsApplied` set at builder time — the tokenizer does not drop or
// coalesce whitespace.
//
// Schema resolution: Token.schemaKind is filled by the tokenizer via
// `GrammarSchema::lookupLexeme`, picking the highest-priority
// candidate. Scope filtering and contextual-keyword demotion remain
// `TreeBuilder::pushToken`'s job — those need the schema cursor +
// scope stack, which the tokenizer doesn't track.
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
    // emitted during tokenization (`P_IllegalChar`, `P_MalformedNumber`,
    // and any future tokenizer-emitted codes).
    [[nodiscard]] TokenizeResult tokenize() &&;

private:
    std::shared_ptr<SourceBuffer>        source_;
    std::shared_ptr<GrammarSchema const> schema_;
    std::unique_ptr<DiagnosticReporter>  reporter_;
};

} // namespace dss
