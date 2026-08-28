#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "tokenizer/token_stream.hpp"

#include <cstdint>
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
    // `budget` is REQUIRED and deliberately carries no default argument.
    // It used to be a `DiagnosticReporter::Config diagConfig = {}`, and every
    // one of the three production construction sites took that default — so
    // the parameter existed, reached nothing, and the tokenizer capped at the
    // library's 1000/50 no matter what the operator asked for
    // (D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE). A caller with no
    // operator budget in scope says so out loud with
    // `DiagnosticBudget::libraryDefault()`.
    //
    // `initialMode` (optional): the lexer mode the BOTTOM scan frame starts in.
    // Default `InvalidLexerMode` ⇒ `main`, which is every whole-FILE caller and
    // is byte-identical to the behaviour before this parameter existed.
    //
    // ★★ IT EXISTS FOR FRAGMENTS, AND THE FIRST ONE IS AN EMBEDDED ASSEMBLY
    // TEMPLATE. A `.s` file and an `__asm__` template are the same dialect and
    // the same grammar, but NOT the same lexical surface: ✔MEASURED on gcc
    // 13.3.0 / clang 18.1.3, `%` is the register sigil in a `.s`, a literal in
    // a BASIC template and an operand introducer in an EXTENDED one. A dialect
    // says so with a per-mode `tokens` override and names the mode in
    // `assembly.templateLexerMode`; the fragment's parse entry passes it here.
    // ⚠ The mode is the bottom frame, not a push — see tokenize()'s comment for
    // why a pushed mode is silently wrong from the first newline onward. An id
    // this schema does not declare is a FATAL caller defect, never a quiet
    // fallback to `main`.
    //
    // ── [[D-PP-PASTE-REJECTS-A-VALID-PREPROCESSING-NUMBER]]: `phase` ────────
    //
    // WHICH TRANSLATION PHASE THIS SCAN IS RUNNING AS. It is NOT a language
    // switch and NOT a new config knob — it is the distinction C itself draws
    // between phase 3 (the source is decomposed into PREPROCESSING tokens) and
    // phase 7 (each preprocessing token is converted to a token).
    //
    // ★ WHY THE TOKENIZER HAS TO KNOW. The two phases disagree on exactly one
    // thing here: a PREPROCESSING NUMBER (C 6.4.8) is a much wider grammar than
    // any language's actual numeric literal — `0y1`, `1e` and `1x` are all
    // single valid pp-numbers, and they become errors only when phase 7 tries to
    // convert them. A scanner that knows only the literal grammar STOPS SHORT
    // and hands back two tokens, and every consumer that has to answer "is this
    // ONE preprocessing token?" then gets the wrong answer. ✔MEASURED, gcc
    // 13.3.0 and clang 18.1.3 SEPARATELY: both accept `0 ## y1` and both then
    // report an invalid integer suffix; DSS refused the paste outright.
    //
    // ⚠ The pp-number GRAMMAR is still entirely config-derived — the digit
    // classes, the fraction point and the exponent letters come from
    // `numberStyle`, the identifier bytes from `identifierClass`. This parameter
    // selects a PHASE, it does not encode one language's spelling of anything,
    // and a language whose schema declares no numeric grammar cannot reach the
    // rule at all.
    enum class Phase : std::uint8_t {
        // Phase 7: a numeric literal is scanned by the language's own literal
        // grammar and stops where that grammar stops. The default, and
        // byte-identical to the behaviour before this parameter existed.
        Tokens,
        // Phase 3: a numeric literal absorbs its maximal PREPROCESSING-NUMBER
        // tail. A run the language's literal grammar does not fully accept comes
        // back as ONE token flagged malformed (P_MalformedNumber) rather than as
        // a silent split — the same "committed, so never split" doctrine the
        // float continuation already applies.
        PreprocessingTokens,
    };

    Tokenizer(std::shared_ptr<SourceBuffer>        src,
              std::shared_ptr<GrammarSchema const> schema,
              DiagnosticBudget                     budget,
              LexerModeId                          initialMode = {},
              Phase                                phase = Phase::Tokens);

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
    LexerModeId                          initialMode_{};
    Phase                                phase_ = Phase::Tokens;
};

} // namespace dss
