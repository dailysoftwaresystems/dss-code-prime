#pragma once

// Config-driven C preprocessor (FC13). The WHOLE pass is config-SELECTED:
// a language opts in via a `preprocess` block in its `.lang.json`
// (`GrammarSchema::preprocess().enabled`); a language without the block
// (toy / tsql-subset) gets a strict IDENTITY pass (token stream in == out).
// NO code here branches on the language name -- the directive vocabulary
// (`#`, `define`, `undef`, `include`, the quote/angle include openers) is
// read entirely from `PreprocessConfig`.
//
// Representation (locked design): TEXT-CONCAT + LINE-MAP + token-level macro
// expansion in ONE buffer. `Token` (16B) and `SourceSpan` (8B) carry no
// buffer id, and the parser hardwires one SourceBuffer per parse, so tokens
// CANNOT be spliced across buffers. Instead the pass:
//   1. Builds ONE synthesized SourceBuffer by recursively concatenating the
//      main file's text + each quote-`#include "h"`'d header's (already
//      preprocessed) text. Angle includes (`#include <h>`) are LEFT in place
//      for the existing post-parse import resolver. A LINE-MAP records, for
//      every synthesized byte range, the ORIGIN (file + offset) so a
//      diagnostic on the synth buffer remaps to the real header:line.
//   2. Tokenizes the synth buffer ONCE -- every token's span is valid in that
//      single buffer.
//   3. Runs the macro pass: builds the table from `#define`/`#undef` (OBJECT-
//      and FUNCTION-like, FC13 cycle 2), then stream-expands invocations by
//      splicing the replacement tokens' spans (valid in the SAME buffer -- the
//      `#define` line is physically present). A function-like call collects its
//      paren-balanced arguments, pre-expands each, substitutes them into the
//      replacement, then RESCANS. The blue-paint self-reference guard and
//      directive-line removal apply uniformly. (No `#`/`##` operators yet.)
//   4. Re-packages the surviving tokens into a fresh TokenStream.
//
// FC14 (D-PP-CONDITIONAL-COMPILATION) adds CONDITIONAL compilation:
// `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif` + the `defined` operator. A
// condition stack tracks branch state; a dead branch's tokens are simply NOT
// emitted into the body (elision precedes macro expansion). The `#if`/`#elif`
// controlling expression is an integer-constant-expression evaluated by
// `pp_if_eval` (config-driven precedence + the shared const-eval arithmetic
// core). The whole vocabulary (directive words + `defined`) is config-driven.
//
// FAIL-LOUD on every unsupported construct (function-like macro arity mismatch,
// unterminated invocation, variadic/duplicate-parameter/malformed parameter
// list, incompatible redefinition, missing quote include, include recursion
// overflow, an unterminated/mismatched conditional, a non-ICE / sizeof / float /
// string operand in `#if`) -- never a silent pass-through or miscompile. Out of
// scope (pinned, not built): `##`/`#`/predefined macros (FC15-area).

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "tokenizer/token_stream.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace dss {

// One contiguous run of the synthesized buffer that came VERBATIM from a
// single origin buffer. The synth buffer is a concatenation of such runs
// (header text spliced in where a quote-include was), so a binary search on
// `synthStart` resolves any synth offset to its origin.
struct DSS_EXPORT LineMapSegment {
    ByteOffset                    synthStart = 0;   // inclusive, synth coords
    ByteOffset                    synthEnd   = 0;   // exclusive, synth coords
    std::shared_ptr<SourceBuffer> origin;           // the real file this run came from
    ByteOffset                    originStart = 0;   // origin offset of synthStart
};

// synth-offset -> (origin buffer, origin offset). Built by the synth-buffer
// builder; consumed to remap diagnostics off the synth buffer back onto the
// real header/main file. A run is a VERBATIM copy (offsets advance 1:1 within
// a segment), so the origin offset of a synth offset `o` in segment `s` is
// `s.originStart + (o - s.synthStart)`. Offsets that land in SYNTHESIZED
// glue (e.g. an injected newline between concatenated files) map to the
// nearest preceding segment's origin -- good enough for attribution and never
// out of bounds.
class DSS_EXPORT LineMap {
public:
    void addSegment(LineMapSegment seg) { segments_.push_back(std::move(seg)); }

    // Resolve a synth offset. Returns {origin buffer (may be null if the map
    // is empty), origin offset}. Never aborts.
    struct Resolved {
        SourceBuffer const* origin = nullptr;
        ByteOffset          offset = 0;
    };
    [[nodiscard]] Resolved resolve(ByteOffset synthOffset) const noexcept;

    [[nodiscard]] std::span<LineMapSegment const> segments() const noexcept {
        return segments_;
    }
    [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }

private:
    std::vector<LineMapSegment> segments_;
};

// The product of a preprocess run.
struct DSS_EXPORT PreprocessResult {
    // The single synthesized buffer the tokens reference. Kept alive here
    // (and registered with the diagnostic BufferRegistry) so spans stay valid.
    std::shared_ptr<SourceBuffer> synthBuffer;

    // The preprocessed tokens (directives removed, macros expanded),
    // Eof-terminated, all referencing `synthBuffer`. A `TokenStream` for the
    // parser is built from a COPY of this via `TokenStream::fromTokens` -- the
    // vector is retained (not moved into a one-shot stream) so the FC2
    // type-name oracle's one-shot REPARSE can rebuild an identical stream
    // without re-running the whole preprocess.
    std::vector<Token> tokens;

    // synth-offset -> origin map for diagnostic remapping.
    LineMap lineMap;

    // The DISTINCT origin buffers the line-map references, EXCLUDING the synth
    // buffer: the ORIGINAL main file + every quote-`#include`'d header. After
    // `makeRemap` redirects a diagnostic off the synth buffer onto its origin
    // (header OR main), those origin buffers must be REGISTERED with the
    // diagnostic `BufferRegistry` for positioned rendering -- otherwise a
    // remapped diagnostic renders as `--> <unknown-buffer:N>`. The CU carries
    // these as `auxiliaryBuffers()`; the driver folds them into the registry
    // next to each tree's own source. Deduped by `SourceBuffer*` identity.
    std::vector<std::shared_ptr<SourceBuffer>> originBuffers;

    // The MAIN source buffer's id (the file passed to `preprocess`). Retained
    // for identification/diagnostics; `makeRemap` now redirects BOTH main- and
    // header-origin diagnostics off the synth buffer onto their real origin
    // buffer (the original main file is one of `originBuffers`), so a
    // main-file error after a leading `#include` reports the ORIGINAL main.c
    // line rather than a splice-shifted synth line.
    BufferId mainSourceId{};

    // PP-phase diagnostics (missing quote include, macro arity mismatch /
    // unterminated invocation / malformed-or-variadic parameter list,
    // incompatible redefinition, recursion overflow, malformed directive).
    // Owned here; the caller folds them into the tree's reporter.
    std::unique_ptr<DiagnosticReporter> diagnostics;

    // TRUE when a FATAL preprocessor backstop fired and TRUNCATED the
    // token stream: the macro-expansion-nesting guard (>256) or the
    // include-nesting guard (possible cycle). Distinct from
    // `diagnostics->hasErrors()`: a RECOVERABLE PP error (missing
    // `#include` file, malformed directive, redefinition) or a folded
    // LEXER error leaves the stream INTACT and parseable, so it does NOT
    // set this. The caller (D-PP-FATAL-HALTS-PARSE) gates the parser on
    // THIS flag — a truncated stream must not be fed to the parser (it
    // produces an inscrutable secondary cascade), but a recoverable PP
    // error must still parse so the parse-level diagnostics surface.
    bool fatal = false;

    // Build a remap closure usable by `DiagnosticReporter::remapBuffers`:
    // it rewrites any diagnostic whose buffer is the synth buffer to the
    // origin (buffer id + offset-shifted span). Diagnostics on other buffers
    // pass through untouched.
    [[nodiscard]] std::function<void(BufferId&, SourceSpan&)> makeRemap() const;
};

// Run the preprocessor over `mainSource` under `schema`. Precondition:
// `schema->preprocess().enabled` is true (the caller gates on it; calling
// with a disabled schema is a usage error and fatal-asserted). `includeDirs`
// is the quote-include search path (the including file's own directory is
// always tried first, mirroring the import resolver).
[[nodiscard]] DSS_EXPORT PreprocessResult preprocess(
    std::shared_ptr<SourceBuffer>        mainSource,
    std::shared_ptr<GrammarSchema const> schema,
    std::span<std::filesystem::path const> includeDirs);

} // namespace dss
