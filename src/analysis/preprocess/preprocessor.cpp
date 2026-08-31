#include "analysis/preprocess/preprocessor.hpp"

#include "core/substrate/checked_file_read.hpp"   // the ONE checked whole-file read
#include "core/substrate/path_identity.hpp"

#include "analysis/preprocess/pp_if_eval.hpp"
#include "core/types/header_case_diagnostic.hpp"   // reportHeaderCaseAmbiguity (the ONE fold-collision emit)
#include "core/types/include_path_resolve.hpp"
#include "core/types/integer_literal_ladder.hpp"  // preprocessorLiteralSignedness (the ONE phase-4 signedness rule the shipped-constant spelling is verified against)
#include "core/types/literal_close_token.hpp"   // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN
#include "core/substrate/phase_timers.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "tokenizer/tokenizer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace dss {


namespace {

[[noreturn]] void ppFatal(char const* what) {
    std::fputs("dss::preprocess fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

constexpr int kMaxIncludeDepth = 64;

// D-CPP-ERROR-WARNING: `sev` is a TRAILING DEFAULTED parameter so every existing
// call site stays byte-identical and keeps its Error severity. Only `#warning`
// (C23 6.10.6 -- translation CONTINUES, so it must never bump `errorCount()`)
// passes anything else; a preprocessor diagnostic is otherwise always an Error.
void emitPP(DiagnosticReporter& rep, DiagnosticCode code, BufferId buffer,
            SourceSpan span, std::string actual,
            DiagnosticSeverity sev = DiagnosticSeverity::Error) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = sev;
    d.buffer   = buffer;
    d.span     = span;
    d.actual   = std::move(actual);
    rep.report(std::move(d));
}

bool isTrivia(Token const& t) {
    return t.coreKind == CoreTokenKind::Whitespace
        || t.coreKind == CoreTokenKind::LineComment
        || t.coreKind == CoreTokenKind::BlockComment
        || isEmptySpace(t.flags);
}
bool isNewline(Token const& t) { return t.coreKind == CoreTokenKind::Newline; }

// ══ D-PERF-PP-OFF-GRAMMAR-BODY-RUN-IS-ONE-TOKEN-PER-CODEPOINT ═══════════════
//
// TRUE when `next` may be folded into `prev` — i.e. the two are consecutive
// codepoints of ONE non-coalesced, off-grammar body run (a comment). The caller
// (`tokenizeToPP`) then widens `prev`'s span over `next` instead of appending a
// second token, so a comment costs ONE token rather than one per character.
//
// ★★ WHY THIS IS THE PREPROCESSOR'S JOB AND NOT A WORKAROUND FOR THE
// TOKENIZER. C 5.1.1.2p1 translation phase 3 is explicit: "Each comment is
// replaced by one space character." Phase 3 belongs to THIS pass, and this
// function sits at the point where the pass materializes its own view of the
// token stream. The tokenizer is RIGHT to emit one token per body codepoint —
// a body mode is a general mechanism, and a string body's codepoints are
// VALUE-BEARING and must stay separable (which is why `defaultToken.coalesce`
// exists and why a comment mode deliberately does not set it). What is wrong is
// carrying per-codepoint granularity through a pass in which every consumer
// already treats the whole run as one space.
//
// ⚠ ✔MEASURED (cycle P34, sqlite 103 TU, Release, Windows/MinGW): of the
// 106.3 M tokens the preprocessor materialized, 93.1 M — 89% — were
// single-CHARACTER comment-body tokens, and the stream handed to the PARSER was
// 86.2 M tokens of which 82.2 M were non-newline trivia. Every one of those
// became an off-grammar AST leaf.
//
// ★ AGNOSTIC BY CONSTRUCTION — no token kind, comment spelling, language,
// architecture or object-format is named. The fold is admitted by exactly two
// schema-supplied facts: the kind is a `lexerModes.<name>.defaultToken.kind`
// (`GrammarSchema::isBodyDefaultKind`, the loader's own single source of truth
// for "off-grammar body token"), and the schema flags it `EmptySpace` (via
// `isTrivia`). A language that spells comments differently folds by the same
// rule; one that declares no such mode is inert here.
//
// ★★ THE FOUR EXCLUSIONS ARE EACH LOAD-BEARING, AND THREE OF THEM ARE THE
// REASON THIS PREDICATE IS NOT SIMPLY `isTrivia && isTrivia`:
//   • SAME KIND ONLY, AND THE KIND IS CARRIED FORWARD UNCHANGED. Widening a
//     span changes the token's LEXEME, and `TreeBuilder::resolveMeaning`
//     resolves a kind through the per-lexeme table first. Folding a
//     mode-OPENING token (`//`, whose meaning is the global table's entry for
//     that exact lexeme) into its body makes that lookup miss, and the
//     builder's drift guard then fatal-aborts — ✔MEASURED, exactly that abort,
//     on the first cut of this change. A body-default kind is resolved by KIND
//     (the synthesis path's `bodyKinds` arm), never by lexeme, so a widened one
//     resolves identically. The opener and closer therefore stay their own
//     tokens; only the BODY folds.
//   • VALUE-BEARING BODIES NEVER FOLD. `isTrivia` is what excludes them: a
//     string / char / header body is a body-default kind too, and its
//     codepoints are decoded downstream. Only a body the schema itself declares
//     `EmptySpace` is foldable.
//   • A NEWLINE NEVER FOLDS. The preprocessor is line-oriented — `firstOnLine`
//     and every `lineEnd` walk read Newline tokens — and `isTrivia` answers
//     TRUE for one (the schema flags `"\n"` `EmptySpace`), so a fold keyed on
//     triviality alone would erase directive-line structure. A newline INSIDE a
//     block comment is a body token, not a Newline token, and folds — which is
//     the pre-existing behaviour this preserves exactly.
//   • A NON-ADJACENT PAIR NEVER FOLDS. Tokens from one tokenize are contiguous
//     by construction, so the test is nearly free; it is here so that a future
//     emitter leaving a gap cannot silently produce a token whose span covers
//     bytes it does not represent.
//
// ★ SPANS ARE PRESERVED EXACTLY. The folded token spans precisely the bytes of
// the run it replaces, so every downstream offset question — the line map, the
// dead-region byte oracle, `__LINE__`, a diagnostic's position, an AST leaf's
// extent — resolves to the same bytes as before. The fold removes TOKENS, never
// bytes and never a byte's attribution.
[[nodiscard]] bool foldsIntoPrecedingBodyRun(GrammarSchema const& schema,
                                             Token const&         prev,
                                             Token const&         next) noexcept {
    return prev.schemaKind.valid()
        && prev.schemaKind == next.schemaKind
        && schema.isBodyDefaultKind(prev.schemaKind)
        && isTrivia(prev) && !isNewline(prev)
        && prev.span.end() == next.span.start();
}

// Phase-2 line-continuation splice: delete every backslash-newline pair,
// recording 1:1 line-map segments per verbatim run so synth offsets remap to
// the ORIGINAL file. Appends to out/map based at out.size().
void appendWithContinuationSplice(std::string_view text,
                                  std::shared_ptr<SourceBuffer> const& origin,
                                  ByteOffset originBase,
                                  std::string& out,
                                  LineMap& map) {
    std::size_t runStart = 0;
    std::size_t i        = 0;
    const char backslash = '\\';
    const char newline   = '\n';
    const char carriage  = '\r';
    auto flushRun = [&](std::size_t end) {
        if (end <= runStart) return;
        LineMapSegment seg;
        seg.synthStart  = static_cast<ByteOffset>(out.size());
        out.append(text.substr(runStart, end - runStart));
        seg.synthEnd    = static_cast<ByteOffset>(out.size());
        seg.origin      = origin;
        seg.originStart = static_cast<ByteOffset>(originBase + runStart);
        map.addSegment(std::move(seg));
    };
    while (i < text.size()) {
        if (text[i] == backslash && i + 1 < text.size()
            && text[i + 1] == newline) {
            flushRun(i);
            i += 2;
            runStart = i;
        } else if (text[i] == backslash && i + 2 < text.size()
                   && text[i + 1] == carriage && text[i + 2] == newline) {
            flushRun(i);
            i += 3;
            runStart = i;
        } else {
            ++i;
        }
    }
    flushRun(text.size());
}

struct PPToken {
    Token            tok;
    std::string_view text;
};

// [[D-PP-PASTE-REJECTS-A-VALID-PREPROCESSING-NUMBER]]: this is PHASE 3, so the
// scan forms PREPROCESSING tokens. `0y1` / `1e` / `1x` are single valid
// pp-numbers (C 6.4.8) and come back as ONE malformed token rather than as a
// number plus an identifier — which is what lets `##` answer "is the product a
// single preprocessing token?" with the scanner's answer instead of a private
// re-implementation of the number grammar.
std::vector<PPToken> tokenizeToPP(
    std::shared_ptr<SourceBuffer> const& buffer,
    std::shared_ptr<GrammarSchema const> const& schema,
    DiagnosticReporter& rep) {
    // The tokenizer's own reporter is drained into `rep` on the very next
    // lines, so its budget IS `rep`'s budget -- taken from the destination
    // rather than re-supplied, which makes the two agree by construction
    // (D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE). `DiagnosticBudget`
    // reads only the volume axes, so `rep`'s policy is NOT re-applied here.
    Tokenizer tk{buffer, schema, DiagnosticBudget{rep.config()},
                LexerModeId{}, Tokenizer::Phase::PreprocessingTokens};
    auto result = std::move(tk).tokenize();
    if (result.diagnostics) {
        for (auto const& d : result.diagnostics->all()) rep.report(d);
    }
    std::vector<PPToken> out;
    while (!result.stream.isAtEnd()) {
        Token t = result.stream.advance();
        if (t.coreKind == CoreTokenKind::Eof) break;
        // D-PERF-PP-OFF-GRAMMAR-BODY-RUN-IS-ONE-TOKEN-PER-CODEPOINT: one
        // comment is ONE token here, not one per character (C 5.1.1.2p1 phase
        // 3). See `foldsIntoPrecedingBodyRun` for why the opener/closer and
        // every value-bearing body are excluded, and why the folded token keeps
        // the run's own kind.
        if (!out.empty()
            && foldsIntoPrecedingBodyRun(*schema, out.back().tok, t)) {
            Token& prev     = out.back().tok;
            prev.span       = SourceSpan::of(prev.span.start(), t.span.end());
            prev.flags      = prev.flags | t.flags;
            out.back().text = buffer->slice(prev.span);
            continue;
        }
        out.push_back(PPToken{t, buffer->slice(t.span)});
    }
    return out;
}

// D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN: the source end offset of the coalesced
// literal whose BODY token is `toks[bodyIdx]` — i.e. the offset PAST its close
// delimiter (`"` for a quote include, `>` for an angle one).
//
// LOAD-BEARING, and the reason this is NOT just `body.span.end()`: the include
// arms feed this offset to `copiedUpTo`, the point at which verbatim source
// copying RESUMES after a spliced-away `#include`. Stop one byte short and the
// closing delimiter is left in the emitted text, where a stray `"` pairs with
// the NEXT quote in the file and swallows everything between — a SILENT
// miscompile with no diagnostic anywhere. The literal's BODY span deliberately
// did not change when the closer got its own token, so this offset must be read
// off the CLOSER token; the closer's KIND comes from the schema (keyed on the
// body token's own kind), never from the `"`/`>` byte.
//
// Falls back to the body end when the language declares no closer for this body
// mode, or when the expected closer is absent. Absent means the literal was
// UNTERMINATED (the tokenizer emits the body, then the closer only if it saw
// one) — in which case the body swallowed the rest of the file, every caller's
// resolve step fails loud on the garbage filename, and the main macro pass
// re-reports the tokenizer's P_UnterminatedString that this pre-scan's throwaway
// reporter discarded. So the fallback never produces a silent splice.
[[nodiscard]] ByteOffset literalEndPastCloser(GrammarSchema const&        schema,
                                              std::vector<PPToken> const& toks,
                                              std::size_t                 bodyIdx) {
    const ByteOffset bodyEnd = toks[bodyIdx].tok.span.end();
    const SchemaTokenId closeKind =
        closeTokenForCoalescedBody(schema, toks[bodyIdx].tok.schemaKind);
    const std::size_t closeIdx = bodyIdx + 1;
    if (closeKind.valid() && closeIdx < toks.size()
        && toks[closeIdx].tok.schemaKind == closeKind
        && toks[closeIdx].tok.span.start() == bodyEnd) {
        return toks[closeIdx].tok.span.end();
    }
    return bodyEnd;
}

} // namespace

Token const& PreprocessResult::eofToken() const {
    // Fail LOUD and NAMED rather than reading past the end. `preprocess()`'s
    // single exit (`establishResultContract`) guarantees both conditions for
    // every result it hands out, so reaching either message means a
    // `PreprocessResult` was produced by something other than that exit.
    if (tokens.empty()) {
        ppFatal("PreprocessResult::eofToken: the token vector is EMPTY - every "
                "result `preprocess()` returns is Eof-terminated by contract, "
                "so this one did not come from that function's single exit");
    }
    if (tokens.back().coreKind != CoreTokenKind::Eof) {
        ppFatal("PreprocessResult::eofToken: the token vector is NOT "
                "Eof-terminated - its last token is not CoreTokenKind::Eof");
    }
    return tokens.back();
}

namespace {

// ── [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: THE ONE FORWARD REWRITE ──────
//
// Move ONE (buffer, span) off the synth buffer onto the file it belongs to.
// Shared by both closures below so the position answer has a single
// implementation and the diagnostic form cannot drift from the position form.
//
// ★★ THE VALIDATION, AND WHY IT IS AN ABORT AND NOT A CLAMP. `LineMap::originOf`
// is total and every one of its answers is in bounds by construction, so
// `Escaped` means a segment does not describe its own origin — a compiler bug,
// not a source error, and nothing downstream can recover a correct position from
// it. TF-C80 met the same class at the CONSUMER and clamped it; that removed a
// heap over-read and an abort but left the diagnostic pointing at end-of-file,
// which is how a WRONG position became an INVISIBLE one. So the producer refuses
// instead, LOUD and named, exactly where the wrong number would otherwise be
// minted. ⚠ This is NOT the past-end product span that used to reach here: that
// case now has a real answer (`SynthOriginKind::Expansion`) and never lands on
// this branch, which is what keeps the abort unreachable from user input.
void remapOnePosition(LineMap const& map, BufferId synthId, BufferId& buffer,
                      SourceSpan& span) {
    if (!(buffer == synthId)) return;
    const SynthOrigin s = map.originOf(span.start());
    const SynthOrigin e = map.originOf(span.end());
    if (s.kind == SynthOriginKind::Escaped
        || e.kind == SynthOriginKind::Escaped) {
        ppFatal("PreprocessResult::makeRemap: a synth offset resolved PAST THE "
                "END of the origin buffer it was attributed to. Every arm of "
                "LineMap::originOf is in bounds by construction, so a line-map "
                "segment does not describe its own origin - this is a compiler "
                "bug, not a source error. Refusing rather than clamping: a "
                "clamped position renders plausibly at end-of-file and hides "
                "the defect (see D-DIAG-RENDERER-PAST-END-SPAN-HEAP-OVERREAD)");
    }
    if (s.origin == nullptr) return;   // no position at all -> leave on synth.
    buffer = s.origin->id();
    if (s.kind == SynthOriginKind::Expansion) {
        // ★ A PRODUCT SPAN HAS NO EXTENT IN ANY FILE, so it gets none. The
        // expansion site is a POINT, and widening it to `[site, whatever the
        // END offset resolved to)` underlines unrelated source: the exclusive
        // end of a minted run is one past the run and belongs to no run, so it
        // resolves as end-of-unit and the caret grew to swallow the rest of the
        // line (✔MEASURED on `int CAT(0, x1) = 2;` — 15 carets under
        // `STR(name) = 1;` before this). ✔MEASURED clang 18.1.3 renders exactly
        // one `^` at the expansion site for this case.
        span = SourceSpan::empty(s.offset);
        return;
    }
    if (e.origin == s.origin && e.offset >= s.offset) {
        span = SourceSpan::of(s.offset, e.offset);
    } else {
        span = SourceSpan::empty(s.offset);
    }
}

} // namespace

std::function<void(BufferId&, SourceSpan&)> PreprocessResult::makeRemap() const {
    BufferId const synthId = synthBuffer ? synthBuffer->id() : BufferId{};
    // Redirect EVERY synth-buffer diagnostic onto its real origin buffer --
    // both an included HEADER and the ORIGINAL main file. A header-origin span
    // attributes to the header:line; a main-origin span attributes to the
    // ORIGINAL main.c offset (the line-map already shifted it back across any
    // splice), so a main-file error after a leading `#include` reports the
    // real main.c line, not a synth-shifted one. The caller registers all
    // origin buffers (`PreprocessResult::originBuffers`, surfaced as the CU's
    // `auxiliaryBuffers()`) with the diagnostic registry so the redirected
    // buffer id resolves for rendering. (Previously main-origin diagnostics
    // were kept on the synth buffer -- byte-identical only for a no-include
    // TU -- which mis-positioned a post-include main error: the
    // D-PP-INCLUDE-SPLICE-POSITION-ATTRIBUTION deferral, now closed.)
    // Capture the line-map BY VALUE: the returned closure is stored in the
    // CU sidecar (`ppRemap`) and re-invoked by the FC2 oracle reparse LONG
    // after this PreprocessResult is destroyed, so a pointer into
    // `this->lineMap` would dangle. The copy is a vector of segments
    // (shared_ptr origins), cheap relative to a reparse.
    LineMap mapCopy = lineMap;
    return [synthId, mapCopy = std::move(mapCopy)]
           (BufferId& buffer, SourceSpan& span) {
        remapOnePosition(mapCopy, synthId, buffer, span);
    };
}

// ── [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: the DIAGNOSTIC form ───────────
//
// The same position rewrite, plus the note a macro-expansion product needs to be
// actionable. Separate from `makeRemap` rather than replacing it because the two
// have genuinely different consumers: the LSP's `PreprocessedPositionMap`, the
// shipped-descriptor refs and every post-parse tier convert POSITIONS and have no
// diagnostic to annotate, while the parse tier and the FC2 oracle reparse hand
// whole `ParseDiagnostic`s through. `DiagnosticReporter::remapBuffers` accepts
// either shape and dispatches on the callable's signature, so no call site had to
// learn which one it holds.
//
// ★ THE NOTE IS WHAT MAKES THE POSITION USABLE, and it is measured, not invented.
// ✔MEASURED clang 18.1.3 on `#define STR(x) #x` + `int STR(name) = 1;`: the
// error is reported at the INVOCATION (the `STR` token's own line and column in
// the user's file, one `^` wide), and it carries a `note: expanded from macro
// 'STR'` at the macro's `#define` NAME. gcc 13.3.0 prints the sibling `note: in
// definition of macro 'STR'`. Both name the macro and both give the reader a
// second location; a bare position at the invocation would say "something in
// this macro" and stop there.
// (The two renderings are quoted in prose deliberately: a `file:line:col`
//  written into a source file reads to `scripts/check-plan-citations` as a
//  positional citation, and a guard that learns exceptions is a guard nobody
//  reads.)
//
// ⚠ ONE note, not a chain. The record names the macro whose replacement list
// actually minted the bytes, and the position is the outermost source anchor; the
// INTERMEDIATE levels of `A -> B -> paste` are not recoverable here, because the
// engine expands a finite macro CHAIN ITERATIVELY in one frame (splice + rescan,
// see `expand`) rather than recursively, so there is no stack to read them off.
// A full backtrace needs per-token expansion provenance, which is a larger thing
// than this row and is deliberately NOT faked with a plausible-looking chain.
std::function<void(ParseDiagnostic&)>
PreprocessResult::makeDiagnosticRemap() const {
    BufferId const synthId = synthBuffer ? synthBuffer->id() : BufferId{};
    LineMap mapCopy = lineMap;
    return [synthId, mapCopy = std::move(mapCopy)](ParseDiagnostic& d) {
        // Read the provenance BEFORE the primary span moves off the synth
        // buffer — afterwards the offset is an ORIGIN offset and means something
        // else entirely.
        MacroExpansionSite const* minted = nullptr;
        if (d.buffer == synthId) {
            minted = mapCopy.originOf(d.span.start()).expansion;
        }
        remapOnePosition(mapCopy, synthId, d.buffer, d.span);
        for (RelatedLocation& r : d.related) {
            remapOnePosition(mapCopy, synthId, r.buffer, r.span);
        }
        // Appended AFTER the existing related locations are converted, so the
        // note this adds is not run through the rewrite a second time.
        if (minted == nullptr || !minted->hasDefinition) return;
        BufferId   noteBuffer = synthId;
        SourceSpan noteSpan   = SourceSpan::empty(minted->defOffset);
        remapOnePosition(mapCopy, synthId, noteBuffer, noteSpan);
        if (noteBuffer == synthId) return;   // could not be placed in a real file
        d.related.push_back(RelatedLocation{
            noteBuffer, noteSpan,
            std::string{"expanded from macro '"} + minted->name + "'"});
    };
}

namespace {

// FC14 / c17 (D-PP-CONDITIONAL-INCLUDE-ORDERING): the condition stack frame
// (C 6.10.1). LIFTED to the anonymous namespace (was nested in `MacroExpander`)
// so BOTH the macro-expansion pass AND the SynthBuilder pre-scan share ONE
// frame type + ONE set of transition free functions (`sbHandle*` below). Each
// open `#if`/`#ifdef`/`#ifndef` pushes a frame; `#elif`/`#else` mutate the TOP;
// `#endif` pops. A token (or a gated directive / quote-include) is live iff
// EVERY frame's `thisBranchActive` is true.
struct CondFrame {
    bool enclosingActive;   // was the stack active when this frame opened?
    bool anyBranchTaken;    // has any branch of this group been taken yet?
    bool thisBranchActive;  // is the CURRENT branch the live one?
    bool seenElse;          // has a `#else` been seen in this group?
};

// Which `#if`-family directive opened a frame. The single source of truth:
// `MacroExpander::IfKind` is a `using`-alias of this. Shared by the free
// `sbHandle*` functions below.
enum class SbIfKind { Expr, Ifdef, Ifndef };

// True iff every open conditional frame's current branch is active (empty stack
// => active). The single liveness predicate for both passes.
[[nodiscard]] bool sbStackActive(std::vector<CondFrame> const& stack) {
    for (CondFrame const& f : stack) {
        if (!f.thisBranchActive) return false;
    }
    return true;
}

// The bare-macro-name definedness test shared by the `#ifdef`/`#ifndef` OPEN
// (`sbHandleIf`) AND the C23 `#elifdef`/`#elifndef` continuation
// (`sbHandleElif`): `#ifdef X` / `#elifdef X` == `defined(X)`, `#ifndef X` /
// `#elifndef X` == `!defined(X)` (C 6.10.1). Extract the single bare `Word`
// operand from `[p, end)` (skipping leading non-newline trivia) and return its
// definedness via `isDefinedCb` -- a DIRECT lookup, NO macro expansion of the
// operand (C 6.10.1p1: the name is the operand of `defined`, not expanded);
// `negate` inverts the sense for the `ndef` forms. A malformed operand (no name
// / not a `Word`) fails LOUD (`P_PreprocessorDirective`) and returns false (the
// branch is treated as not-taken, mirroring the pre-existing `#ifdef` handling).
// `directiveWord` is the ACTUAL directive spelling for the malformed message
// (so `#elifdef` with no name says "#elifdef requires a macro name", never
// "#ifdef ..."). Routing the elif-family definedness through here -- NOT
// `evalExprCb` -- is load-bearing: the `#if` expression evaluator would fold a
// bare name to its VALUE (C 6.10.1p4), NOT test its definedness.
[[nodiscard]] bool sbEvalDefinedName(
    std::vector<Token> const& in, std::size_t p, std::size_t end, bool negate,
    char const* directiveWord,
    std::function<std::string_view(Token const&)> const& textOf,
    std::function<bool(std::string_view)> const& isDefinedCb,
    DiagnosticReporter& rep, BufferId diagBuffer) {
    std::size_t q = p;
    while (q < end && isTrivia(in[q]) && !isNewline(in[q])) ++q;
    if (q >= end || isNewline(in[q]) || in[q].coreKind != CoreTokenKind::Word) {
        emitPP(rep, DiagnosticCode::P_PreprocessorDirective, diagBuffer,
               (q < end ? in[q].span : SourceSpan::empty(0)),
               std::string{"#"} + directiveWord + " requires a macro name");
        return false;
    }
    bool const def = isDefinedCb(textOf(in[q]));
    return negate ? !def : def;
}

// `#if EXPR` / `#ifdef NAME` / `#ifndef NAME`: push a new frame onto `stack`.
// The branch is live iff the enclosing context is active AND the condition
// holds. The operand is evaluated ONLY when the enclosing context is active (a
// dead branch's operand is NOT evaluated -- C 6.10.1p6). The `#if EXPR` value
// comes from `evalExprCb` (the caller binds it to its own macro state); the
// `#ifdef`/`#ifndef NAME` definedness from `isDefinedCb`. `textOf` slices a
// token's spelling (a `Token` is a 16B POD that does not carry its own text;
// the caller binds it to its buffer slice). `[in, p, end)` are the operand
// tokens (everything after the directive word up to the line newline). SHARED
// single-impl: `MacroExpander::handleIf` delegates here (Phase 7).
void sbHandleIf(std::vector<CondFrame>& stack, std::vector<Token> const& in,
                std::size_t p, std::size_t end, SbIfKind kind,
                std::function<std::string_view(Token const&)> const& textOf,
                std::function<bool(std::string_view)> const& isDefinedCb,
                std::function<bool(std::vector<Token> const&, std::size_t,
                                   std::size_t)> const& evalExprCb,
                DiagnosticReporter& rep, BufferId diagBuffer) {
    bool const enclosing = sbStackActive(stack);
    bool cond = false;
    if (enclosing) {
        if (kind == SbIfKind::Expr) {
            cond = evalExprCb(in, p, end);
        } else {
            // `#ifdef`/`#ifndef NAME`: the operand is a single macro name.
            // SHARED with the C23 `#elifdef`/`#elifndef` continuation via
            // `sbEvalDefinedName` -- a malformed operand fails loud there and
            // returns false, so a malformed `#ifdef` is still a false (inactive)
            // branch and the frame is STILL pushed below so the matching #endif
            // balances (byte-identical to the pre-refactor inline logic).
            cond = sbEvalDefinedName(in, p, end, kind == SbIfKind::Ifndef,
                                     kind == SbIfKind::Ifdef ? "ifdef"
                                                             : "ifndef",
                                     textOf, isDefinedCb, rep, diagBuffer);
        }
    }
    stack.push_back(CondFrame{
        /*enclosingActive=*/enclosing,
        /*anyBranchTaken=*/enclosing && cond,
        /*thisBranchActive=*/enclosing && cond,
        /*seenElse=*/false});
}

// `#elif EXPR` / `#elifdef NAME` / `#elifndef NAME` (C23 6.10.1): on the TOP
// frame, take this branch iff the enclosing context is active, NO prior branch
// of this group was taken, AND the controlling condition holds. The `kind`
// selects the condition SOURCE only: `Expr` reads `evalExprCb` (the `#if`
// expression evaluator); `Ifdef`/`Ifndef` read the DIRECT bare-name definedness
// via `sbEvalDefinedName` (C 6.10.1p5: `#elifdef X` == `#elif defined(X)`,
// `#elifndef X` == `#elif !defined(X)` -- the operand is NOT run through the
// expression evaluator, which would fold a bare name to its VALUE). The operand
// is evaluated ONLY when it could be taken (C 6.10.1p6) -- so a dead branch's
// operand (a `#elif 1/0`, or a malformed `#elifdef` with no name) is not
// evaluated + emits no diagnostic. `atSpan` positions the orphan-directive
// diagnostics, which name the actual C directive spelling for `kind`.
void sbHandleElif(std::vector<CondFrame>& stack, std::vector<Token> const& in,
                  std::size_t p, std::size_t end, SbIfKind kind,
                  SourceSpan atSpan,
                  std::function<std::string_view(Token const&)> const& textOf,
                  std::function<bool(std::string_view)> const& isDefinedCb,
                  std::function<bool(std::vector<Token> const&, std::size_t,
                                     std::size_t)> const& evalExprCb,
                  DiagnosticReporter& rep, BufferId diagBuffer) {
    // The canonical C spelling of THIS elif-family directive, for the orphan /
    // after-#else / malformed-name diagnostics (mirrors sbHandleIf's literal
    // "ifdef"/"ifndef" -- the message names the C directive, not the config
    // lexeme). For a plain `#elif` this is "elif", byte-identical to before.
    char const* const word = kind == SbIfKind::Ifdef    ? "elifdef"
                             : kind == SbIfKind::Ifndef  ? "elifndef"
                                                         : "elif";
    if (stack.empty()) {
        emitPP(rep, DiagnosticCode::P_PreprocessorDirective, diagBuffer, atSpan,
               std::string{"#"} + word + " without a matching #if");
        return;
    }
    CondFrame& f = stack.back();
    if (f.seenElse) {
        emitPP(rep, DiagnosticCode::P_PreprocessorDirective, diagBuffer, atSpan,
               std::string{"#"} + word + " after #else");
        return;
    }
    // PRESERVE the update ORDER: latch a prior active branch FIRST, then compute
    // mayTake, THEN evaluate the (possibly-taken) operand, THEN set + re-latch.
    // Moving the latch after mayTake would re-open a taken-once miscompile.
    f.anyBranchTaken = f.anyBranchTaken || f.thisBranchActive;
    bool const mayTake = f.enclosingActive && !f.anyBranchTaken;
    bool cond = false;
    if (mayTake) {
        // SWAP only the condition SOURCE by kind -- the frame-transition logic
        // is identical for `#elif` and the C23 defined-forms.
        cond = (kind == SbIfKind::Expr)
                   ? evalExprCb(in, p, end)
                   : sbEvalDefinedName(in, p, end, kind == SbIfKind::Ifndef,
                                       word, textOf, isDefinedCb, rep,
                                       diagBuffer);
    }
    f.thisBranchActive = mayTake && cond;
    f.anyBranchTaken   = f.anyBranchTaken || f.thisBranchActive;
}

// `#else`: take this branch iff the enclosing context is active and no prior
// branch of this group was taken.
void sbHandleElse(std::vector<CondFrame>& stack, SourceSpan at,
                  DiagnosticReporter& rep, BufferId diagBuffer) {
    if (stack.empty()) {
        emitPP(rep, DiagnosticCode::P_PreprocessorDirective, diagBuffer, at,
               "#else without a matching #if");
        return;
    }
    CondFrame& f = stack.back();
    if (f.seenElse) {
        emitPP(rep, DiagnosticCode::P_PreprocessorDirective, diagBuffer, at,
               "#else after #else");
        return;
    }
    f.anyBranchTaken   = f.anyBranchTaken || f.thisBranchActive;
    f.seenElse         = true;
    f.thisBranchActive = f.enclosingActive && !f.anyBranchTaken;
    f.anyBranchTaken   = true;
}

// `#endif`: pop the top frame.
void sbHandleEndif(std::vector<CondFrame>& stack, SourceSpan at,
                   DiagnosticReporter& rep, BufferId diagBuffer) {
    if (stack.empty()) {
        emitPP(rep, DiagnosticCode::P_PreprocessorDirective, diagBuffer, at,
               "#endif without a matching #if");
        return;
    }
    stack.pop_back();
}

// C21 (D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE / FINDING-B) → TF-C74.
//
// The per-object-format availability predicate for a config PREDEFINED macro
// USED TO LIVE HERE, as a shared helper the four predefine seed sites each
// called: the include-gating pre-scan's definedness oracle (`sbNameDefined`),
// the value-seed prefix builder, the `MacroExpander` `predefined_` seed, and
// the function-like "<built-in>" prologue. It is now DELETED. Four call sites
// of one predicate is one careless edit away from three call sites and a
// fourth that forgot, and a divergence between the pre-scan's seed and the
// authoritative `predefined_` set is a SILENT P0016 seam (the pre-scan
// resolving a value-gated include the authoritative pass reads dead).
//
// The filter now runs EXACTLY ONCE, inside `mergePredefinedMacros` (bottom of
// this file), whose `effective` output is already format-resolved; all four
// sites iterate that ONE list. There is deliberately no named predicate left
// for a fifth site to call — the filter is structurally unrepeatable.

// ── TF-C82 (D-PP-PRAGMA-REGISTRY): the ONE pragma classifier ────────────────────
//
// LONGEST-prefix match of a pragma's leading WORDS against `pragmaEffects`.
// `nullopt` = NO row matched; the caller (not this function) decides loud-vs-
// silent from `unknownPragmaIsError`, so recognition and policy stay separable.
//
// Longest-wins rather than first-wins is deliberate: a future
// `["clang","diagnostic","push"]` row must be able to refine the broader
// `["clang","diagnostic"]` row without either being shadowed by DECLARATION
// ORDER — the same reason the loader rejects a duplicate prefix outright.
//
// TF-C87 moved this block UP from just below `SynthBuilder` (a pure move, no
// logic change) because the include-guard detector below is its second reader.
struct PragmaMatch {
    PragmaEffect effect = PragmaEffect::Unsupported;
    std::size_t  words  = 0;   // how many leading words the matched row consumed
};
[[nodiscard]] inline std::optional<PragmaMatch>
matchPragmaEffect(PreprocessConfig const&      cfg,
                  std::span<std::string const> words) {
    std::optional<PragmaMatch> best;
    for (PragmaEffectRow const& row : cfg.pragmaEffects) {
        if (row.prefix.empty() || row.prefix.size() > words.size()) continue;
        bool ok = true;
        for (std::size_t i = 0; i < row.prefix.size(); ++i) {
            if (words[i] != row.prefix[i]) { ok = false; break; }
        }
        if (!ok) continue;
        if (!best.has_value() || row.prefix.size() > best->words) {
            best = PragmaMatch{row.effect, row.prefix.size()};
        }
    }
    return best;
}

// ══ TF-C87 (D-PP-INCLUDE-REENTRY-GUARD-AWARE) ═══════════════════════════════
//
// ★ WHAT THIS REPLACES. `includeStack` used to REFUSE re-entry into any header
// already on the stack, with `P_PreprocessorIncludeError` "circular include of
// X". That rejects LEGAL, STANDARD-CONFORMING C. MEASURED on the macho corpus
// leg at 5093341:
//     mach/mach_types.h -> mach/task_policy.h -> mach/mach_types.h
// terminates perfectly under a real cpp, because the second entry hits
// `#ifndef _MACH_MACH_TYPES_H_` (already defined by the first) and expands to
// nothing. cpp has NO refuse-re-entry rule at all — it has a NESTING DEPTH
// limit, and the include guard is what terminates the cycle. That single false
// positive was the sole cause of the leg's 4 residual `F_ShippedHeaderNotFound`.
//
// ★ THE MODEL NOW. Re-entry is PERMITTED for a header that carries a MACRO
// include guard, because the guard is what makes the repeat expansion empty:
// `localMacros` is SHARED across the whole builder tree, so on re-entry the
// guard's controlling name is already defined, the whole body reads DEAD, no
// nested include inside it resolves, and the recursion converges after exactly
// one extra level. The AUTHORITATIVE `MacroExpander` pass then elides the
// duplicated text for the same reason. `kMaxIncludeDepth` is the loud backstop
// for anything that still recurses.
//
// ★ WHY THE DETECTOR IS DELIBERATELY GENEROUS. Guard DETECTION GATES re-entry,
// so an unrecognised but LEGAL guard becomes a refused include — and that
// presents to the user as a compiler bug. Under-recognition is therefore the
// DANGEROUS direction. Over-recognition is the SAFE one, and only because
// re-entry is PERMITTED rather than SKIPPED: if the "guard" turns out not to be
// one, the body is spliced again and the REAL conditional logic decides, with
// the depth cap as the backstop. Had this skipped instead, over-recognition
// would silently DROP content a real cpp would have included — the one outcome
// worth engineering against. So the rule below is the most generous one that
// still says NO for a header with no include-once mechanism at all.
//
// ★ THE RULE, and why it needs no `!`/`defined` parsing:
//
//     A header carries a MACRO include guard iff, for some name N, the header
//     `#define`s N INSIDE a conditional region whose CONTROLLING LINE mentions N.
//
// "Mentions" is purely lexical — is the Word `N` among the controlling line's
// tokens — which is what makes this cover every guard SHAPE MEASURED in the wild
// with ONE rule and NO shape vocabulary:
//     #ifndef N / #define N                            (canonical; 2942 SDK, 35 sqlite)
//     #if !defined(N) / #define N                      (10 SDK, 1 sqlite)
//     #if !defined N   (no parens)                     (10+ SDK occurrences)
//     #if !defined(A) && !defined(B) && !defined(N)    (pcap/bpf.h)
//     #if defined(_WIN32) && !defined(N)               (sqlite ext/misc/windirent.h)
//     #if !defined(N) && defined(SQLITE_ENABLE_SESSION) (sqlite ext/session/*)
//     ...with the `#define` NOT on the next line       (15 measured, up to 5 lines)
//     ...with the guard NOT the first conditional      (11 measured; netinet6/in6.h
//                                                       opens with an umbrella check)
// A future spelling this project has never seen is covered too, as long as the
// guard names the macro it tests — which is what a guard IS.
//
// ★ AGNOSTIC. Every directive word comes from `PreprocessConfig`
// (`ifDirective`/`ifndefDirective`/`elif*`/`endifDirective`/`defineDirective`/
// `pragmaDirective`); `#pragma once` is recognised through the `pragmaEffects`
// REGISTRY's `includeOnce` verb, never the word `once`. No header name, no path,
// no SDK knowledge appears anywhere in here.
enum class IncludeOnceMechanism : std::uint8_t {
    // No include-once mechanism found. Re-entry is a real infinite cycle.
    None,
    // A macro include guard (any spelling). Re-entry is safe — the guard empties it.
    MacroGuard,
    // A `#pragma` whose registry row declares the file include-once. DSS has not
    // built include-once dedup, so this is refused — but with its OWN message.
    OncePragma,
};

// Classify `buf` (a header's raw file text). Cheap enough to run on the refusal
// path only, which is where it is called from — a header must ALREADY be on the
// include stack before anyone asks.
[[nodiscard]] IncludeOnceMechanism
detectIncludeOnceMechanism(std::shared_ptr<SourceBuffer> const&        buf,
                           std::shared_ptr<GrammarSchema const> const& schema) {
    PreprocessConfig const& cfg = schema->preprocess();
    if (!cfg.enabled || !buf) return IncludeOnceMechanism::None;
    const auto hashKind = schema->schemaTokens().find(cfg.directiveIntroToken);
    if (!hashKind.valid()) return IncludeOnceMechanism::None;

    // Phase-2 continuation splice FIRST, exactly as `SynthBuilder::build` does:
    // a multi-line `#if !defined(A) && \` … `!defined(N)` is ONE logical
    // directive line, and harvesting names per PHYSICAL line would miss `N` —
    // i.e. would UNDER-recognise, the dangerous direction. The line map is a
    // throwaway: nothing here is emitted and no offset here reaches a diagnostic.
    std::string spliced;
    LineMap     throwawayMap;
    appendWithContinuationSplice(buf->text(), buf, 0, spliced, throwawayMap);
    auto scanBuf = SourceBuffer::fromString(spliced, std::string{buf->name()});

    // A THROWAWAY reporter: a tokenizer complaint about a header this pass is
    // only INSPECTING is not this detector's to report. The authoritative pass
    // re-tokenizes the same bytes and owns every diagnostic about them.
    DiagnosticReporter scratch;
    auto const         toks = tokenizeToPP(scanBuf, schema, scratch);

    // One frame per OPEN conditional group, holding the Word lexemes that appear
    // in that group's controlling lines. `#elif`/`#elifdef`/`#elifndef` extend
    // the CURRENT group's set (they are further controlling expressions of the
    // same group); `#else` adds none; `#endif` pops.
    std::vector<std::vector<std::string_view>> control;
    bool                                       sawOncePragma = false;

    auto firstOnLine = [&](std::size_t idx) {
        for (std::size_t p = idx; p-- > 0;) {
            if (isNewline(toks[p].tok)) return true;
            if (!isTrivia(toks[p].tok)) return false;
        }
        return true;
    };
    auto lineEndOf = [&](std::size_t start) {
        std::size_t e = start;
        while (e < toks.size() && !isNewline(toks[e].tok)) ++e;
        return e;   // EXCLUSIVE of the newline
    };
    auto wordsIn = [&](std::size_t from, std::size_t to) {
        std::vector<std::string_view> out;
        for (std::size_t p = from; p < to; ++p) {
            if (toks[p].tok.coreKind == CoreTokenKind::Word) {
                out.push_back(toks[p].text);
            }
        }
        return out;
    };

    for (std::size_t i = 0; i < toks.size(); ++i) {
        if (toks[i].tok.schemaKind != hashKind || !firstOnLine(i)) continue;
        std::size_t j = i + 1;
        while (j < toks.size() && isTrivia(toks[j].tok)) ++j;
        if (j >= toks.size()) break;
        std::string_view const  dirWord = toks[j].text;
        std::size_t const       lineEnd = lineEndOf(i);
        std::size_t const       opStart = j + 1;

        if (dirWord == cfg.ifDirective || dirWord == cfg.ifdefDirective
            || dirWord == cfg.ifndefDirective) {
            control.push_back(wordsIn(opStart, lineEnd));
        } else if (dirWord == cfg.elifDirective
                   || (!cfg.elifdefDirective.empty()
                       && dirWord == cfg.elifdefDirective)
                   || (!cfg.elifndefDirective.empty()
                       && dirWord == cfg.elifndefDirective)) {
            if (!control.empty()) {
                auto more = wordsIn(opStart, lineEnd);
                control.back().insert(control.back().end(), more.begin(),
                                      more.end());
            }
        } else if (dirWord == cfg.endifDirective) {
            // An UNBALANCED `#endif` (more closes than opens) is a malformed
            // header the authoritative pass reports; here it must not underflow.
            if (!control.empty()) control.pop_back();
        } else if (dirWord == cfg.defineDirective) {
            std::size_t p = opStart;
            while (p < lineEnd && isTrivia(toks[p].tok)) ++p;
            if (p < lineEnd && toks[p].tok.coreKind == CoreTokenKind::Word) {
                std::string_view const subject = toks[p].text;
                for (auto const& frame : control) {
                    for (std::string_view n : frame) {
                        if (n == subject) return IncludeOnceMechanism::MacroGuard;
                    }
                }
            }
        } else if (!cfg.pragmaDirective.empty()
                   && dirWord == cfg.pragmaDirective) {
            std::vector<std::string> words;
            for (std::size_t p = opStart; p < lineEnd; ++p) {
                if (isTrivia(toks[p].tok)) continue;
                if (toks[p].tok.coreKind == CoreTokenKind::Eof) continue;
                words.emplace_back(toks[p].text);
            }
            auto const m = matchPragmaEffect(cfg, words);
            if (m.has_value() && m->effect == PragmaEffect::IncludeOnce) {
                // RECORDED, not returned: a header carrying BOTH mechanisms
                // (MEASURED: `MacTypes.h`, `mach/arm/traps.h`) must be reported
                // as GUARDED, because the macro guard is the one DSS can actually
                // honour — so the walk continues and a later guard hit wins.
                // Deliberately NOT gated on the conditional stack: a `#pragma
                // once` inside a conditional is still this file's declaration
                // about itself, and reading it as ABSENT is the dangerous
                // direction.
                sawOncePragma = true;
            }
        }
        i = lineEnd;   // ++i steps past the newline
    }
    return sawOncePragma ? IncludeOnceMechanism::OncePragma
                         : IncludeOnceMechanism::None;
}

// ── The per-FILE pre-scan memo ───────────────────────────────────────────────
//
// ★★★ D-PERF-PP-EVERY-INCLUDE-RE-READS-AND-RE-TOKENIZES-THE-SAME-HEADER.
// `SynthBuilder::build` opens, phase-2-splices, wraps in a `SourceBuffer` and
// FULLY TOKENIZES its file on EVERY occurrence of an `#include` naming it — per
// translation unit, and again for every TU in the project. Nothing about those
// four steps depends on where the include sits or on what macros are in scope:
// they are a pure function of the file's BYTES. Only the directive WALK that
// follows is state-dependent, and that walk is cheap.
//
// ✔MEASURED 2026-08-25 on the 103-TU sqlite full-source corpus (`--project …
// --config=release --jobs 1`): 1,364 include opens read 109.2 MB and the
// pre-scan tokenized 118.7 MB into 13.2M pp-tokens — 10.2 s of the 16.1 s
// `preprocess-splice` phase — for a distinct header set a small fraction of
// that size. `sqliteInt.h` alone is re-read and re-tokenized once per TU.
//
// So this memo holds, per distinct file, exactly the four pure results:
// the loaded buffer, the continuation-spliced text, its line map, and its
// pp-token vector. It is a MEMO of a pure function, not a cache with a policy:
// there is no eviction, no staleness window, and no way for a hit and a miss to
// disagree — a hit returns the same object the miss would have built.
//
// ⚠ THE KEY IS (identity, size, last-write-time), NOT the path alone. A file
// edited mid-compile MISSES and is re-read rather than silently served stale
// bytes — the fail-loud direction, and it costs two `stat`s against an 80 KB
// read plus a tokenize.
// ⚠ THE ENTRY IS IMMUTABLE ONCE PUBLISHED and handed out as a `shared_ptr<const
// …>`, because the driver preprocesses translation units on a THREAD POOL: a
// reader must never see a half-built entry, and an entry must outlive the
// builder that took it (a `PPToken::text` is a view into `scanBuf`'s bytes, and
// a `LineMapSegment::origin` is a `shared_ptr` the CU keeps). Building happens
// OUTSIDE the lock, so two threads racing on a cold header both do the work and
// the first to publish wins — duplicated work on a cold miss, never a stall.
// ⚠ ONE BUFFER PER DISTINCT FILE, deliberately: the memoized `LineMapSegment`s
// carry `origin` pointers into `source`, so re-using the tokens REQUIRES
// re-using the buffer they were mapped against. Sharing one immutable
// `SourceBuffer` across CUs is also strictly better for the diagnostic
// registry, which used to hold one duplicate per TU of every header.
struct PreScannedFile {
    std::shared_ptr<SourceBuffer> source;    // the file's own bytes
    std::string                   spliced;   // phase-2 continuation splice of them
    LineMap                       localMap;  // spliced -> `source` segments
    std::shared_ptr<SourceBuffer> scanBuf;   // `spliced` as a buffer (owns token text)
    std::vector<PPToken>          toks;      // the ONE tokenize of `spliced`
};

// The memo's identity key. `core::PathIdentity` answers "same file" across
// spellings; size + mtime answer "same bytes".
struct PreScanKey {
    core::PathIdentity      path;
    std::uintmax_t          size = 0;
    std::int64_t            mtime = 0;
    bool operator==(PreScanKey const& o) const noexcept {
        return path == o.path && size == o.size && mtime == o.mtime;
    }
};
struct PreScanKeyHash {
    std::size_t operator()(PreScanKey const& k) const noexcept {
        std::size_t h = std::hash<core::PathIdentity>{}(k.path);
        h ^= std::hash<std::uintmax_t>{}(k.size) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<std::int64_t>{}(k.mtime) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

// Process-lifetime storage. A driver process compiles one invocation and exits,
// so the memo's size is bounded by that invocation's distinct include set.
// `tests` that need isolation do not exist for this: an entry can only ever be
// re-served for a file whose identity, size and mtime all still match.
std::mutex& preScanMemoMutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<PreScanKey, std::shared_ptr<PreScannedFile const>,
                   PreScanKeyHash>&
preScanMemo() {
    static std::unordered_map<PreScanKey, std::shared_ptr<PreScannedFile const>,
                              PreScanKeyHash> m;
    return m;
}

// The counters `PreScanMemoCounters` publishes. RELAXED, deliberately: they are
// an accounting of work already done, never a synchronisation point between the
// threads doing it, so no reader's decision depends on seeing them ordered
// against anything else. Making them acquire/release would put a barrier on the
// hot include path to buy an ordering nobody reads.
std::atomic<std::uint64_t>& preScanBuildCount() {
    static std::atomic<std::uint64_t> n{0};
    return n;
}
std::atomic<std::uint64_t>& preScanHitCount() {
    static std::atomic<std::uint64_t> n{0};
    return n;
}

// Look the file up, or read + splice + tokenize it and publish the result.
// Returns null EXACTLY when `SourceBuffer::fromFile` would have — the callers'
// unreadable-include diagnostics are unchanged.
[[nodiscard]] std::shared_ptr<PreScannedFile const>
preScanIncludeFile(fs::path const& path,
                   std::shared_ptr<GrammarSchema const> const& schema) {
    std::error_code ec;
    PreScanKey key{core::PathIdentity::of(path), 0, 0};
    const auto sz = fs::file_size(path, ec);
    if (!ec) key.size = sz;
    const auto wt = fs::last_write_time(path, ec);
    if (!ec) key.mtime = wt.time_since_epoch().count();
    {
        std::lock_guard<std::mutex> lk{preScanMemoMutex()};
        auto it = preScanMemo().find(key);
        if (it != preScanMemo().end()) {
            preScanHitCount().fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }
    auto buf = SourceBuffer::fromFile(path);
    if (!buf) return nullptr;   // caller emits the unreadable-include diagnostic
    // Counted HERE rather than at the miss, because what this counts is the WORK
    // — the read, the splice and the tokenize below — and a request whose file
    // cannot be read does none of it.
    preScanBuildCount().fetch_add(1, std::memory_order_relaxed);
    auto built = std::make_shared<PreScannedFile>();
    built->source = std::move(buf);
    appendWithContinuationSplice(built->source->text(), built->source, 0,
                                 built->spliced, built->localMap);
    built->scanBuf = SourceBuffer::fromString(
        built->spliced, std::string{built->source->name()});
    // The pre-scan's tokenizer diagnostics are DISCARDED here exactly as they
    // were at the old per-occurrence call site: this pass only INSPECTS the
    // header, and the authoritative pass re-tokenizes the same bytes and owns
    // every diagnostic about them. Memoizing therefore drops nothing.
    DiagnosticReporter scratch;
    built->toks = tokenizeToPP(built->scanBuf, schema, scratch);
    std::shared_ptr<PreScannedFile const> frozen = std::move(built);
    std::lock_guard<std::mutex> lk{preScanMemoMutex()};
    // First publisher wins; a racing loser discards its (identical) copy and
    // serves the published one, so every consumer of one file sees ONE entry.
    auto [it, inserted] = preScanMemo().emplace(key, frozen);
    return it->second;
}

// Recursive synth-text builder. Tokenizes a file to FIND quote includes,
// splices the recursively-preprocessed header text in place of each quote
// include directive, and copies everything else (including angle includes)
// VERBATIM with a 1:1 line-map segment.
// ══ D-PP-PRAGMA-RECOGNIZED-SEMANTICS: the TU's INCLUDE-ONCE REGISTRY ═════════
//
// Every file whose REACHED include-once `#pragma` has fired in ONE translation
// unit. Answers exactly one question — "has this FILE already been spliced?" —
// and it is a different question from the include STACK's "am I inside this file
// right now?". The stack is pushed and popped and catches a CYCLE; this is only
// ever inserted and catches a REPEAT. A header included twice as SIBLINGS is
// never simultaneously on the stack, which is precisely why the pre-existing
// cycle guard did not dedup it and why this row's defect survived that guard.
//
// ★★★ TWO LEVELS, BECAUSE ONE KEY CANNOT ANSWER IT ON THIS HOST — ✔MEASURED
// 2026-08-29 WITH THE STL THAT BUILDS DSS (libstdc++ 13.2, MinGW-w64 UCRT), one
// real `mklink` symlink and one real hard link (same inode, link count 2):
//     fs::is_symlink(link)     no          <- WRONG, it is one
//     fs::read_symlink(link)   ec = "Function not implemented"
//     fs::weakly_canonical     link.h      <- NOT resolved, returned unchanged
//     fs::canonical            link.h      <- NOT resolved either
//     fs::equivalent(h, link)  TRUE
//     fs::equivalent(h, hard)  TRUE
// So `core::PathIdentity` — which normalises a PATH — answers `h.h` twice,
// `./h.h` and `sub/../h.h`, and CANNOT answer a symlink or a hard link here.
// This corrects a premise this row carried: `PathIdentity` was described as
// covering the symlink case, and on this host it does not, because the platform
// STL implements no symlink resolution at all.
//
// ★★★ `fs::equivalent` IS THE AUTHORITY, AND ITS NEGATIVE DIRECTION IS THE
// CONTROL THAT MAKES IT SAFE. It opens both files and compares the filesystem's
// own identity, so it is the STANDARD's spelling of the operator's 2026-08-28
// ruling (IDENTITY, NOT CONTENT) rather than a host-specific shim. ✔MEASURED
// all four directions on the same run: two byte-identical files at different
// paths -> FALSE, same content in a subdirectory -> FALSE, hard link -> TRUE,
// symlink -> TRUE. The false arms matter more than the true ones: a test that
// merged distinct files would DROP TEXT silently.
//
// ★★★ NO SIZE PRE-FILTER, AND THE DRAFT THAT HAD ONE WAS UNSOUND — ✔MEASURED,
// AND IT IS WORTH THE PARAGRAPH BECAUSE THE REASONING LOOKED AIRTIGHT. The first
// version of this class bucketed candidates on `fs::file_size` to avoid a linear
// scan, justified as "two NAMES OF ONE FILE always report the SAME SIZE, so the
// bucket can only ever produce a false NEGATIVE if that invariant broke, which it
// cannot". It CAN, on the host that builds DSS:
//     fs::file_size(h.h)     52
//     fs::file_size(link.h)   0      <- the SAME FILE, through a real symlink
// libstdc++ reads the REPARSE POINT's own size rather than the target's, so the
// two aliases landed in different buckets and the symlink silently failed to
// dedup — a bug whose only symptom was a correct-looking redefinition error.
// The lesson is the one this project keeps paying for: an invariant that "cannot"
// break is a claim about a platform, and it is worth exactly what it was measured
// on. The scan below consults `fs::equivalent` and nothing else.
//
// ★★ IT IS STILL NOT O(n^2), FOR A REASON THAT IS MEASURED RATHER THAN HOPED.
// `files_` counts only files that ACTUALLY FIRED an include-once pragma — not
// headers in general. The TF-C82 reached-set census puts that at ZERO for the
// whole sqlite corpus and at 21 for the macOS SDK, so the scan is over a handful
// of entries, and it is skipped entirely while `files_` is empty. On top of that
// the scan is INCREMENTAL: `checkedUpTo_` remembers how far each spelling has
// already been compared, so a spelling is never re-compared against an entry it
// has already been tested against, and a spelling that HITS is promoted into
// `spellings_` and answers by hash from then on. A plain repeated `#include`
// never touches the filesystem at all.
class IncludeOnceRegistry {
public:
    // Has the file named by `p` (already reduced to `canon`) been spliced once?
    [[nodiscard]] bool alreadySpliced(core::PathIdentity const& canon,
                                      fs::path const&           p) {
        // ZERO COST WHEN THE FEATURE IS UNUSED. No include-once pragma has fired
        // in this TU, so nothing can match and there is nothing to remember —
        // this returns before touching either map. That is the whole sqlite
        // corpus, where the TF-C82 reached-set census measured `once` UNREACHED,
        // and it keeps this row from taxing every `#include` in a build that
        // never uses the pragma.
        if (files_.empty()) return false;
        if (spellings_.count(canon) != 0) return true;
        // How far this spelling has already been compared. `files_` only ever
        // grows, so everything below the watermark is settled and re-testing it
        // could not change the answer.
        std::size_t& from = checkedUpTo_[canon];
        for (std::size_t k = from; k < files_.size(); ++k) {
            std::error_code ec;
            if (fs::equivalent(p, files_[k], ec) && !ec) {
                from = files_.size();
                spellings_.insert(canon);   // memoise: hash hit from now on
                return true;
            }
        }
        from = files_.size();
        return false;
    }

    // Record the file named by `p` as include-once for the rest of this TU.
    void record(core::PathIdentity const& canon, fs::path const& p) {
        if (!spellings_.insert(canon).second) return;   // this spelling is known
        files_.push_back(p);
    }

private:
    std::unordered_set<core::PathIdentity>              spellings_;
    std::unordered_map<core::PathIdentity, std::size_t> checkedUpTo_;
    std::vector<fs::path>                               files_;
};

struct SynthBuilder {
    std::shared_ptr<GrammarSchema const> schema;
    std::span<fs::path const>            includeDirs;
    // System (angle-include) descriptor dirs — threaded so an angle `#include
    // <h>` whose shipped descriptor declares `macros` injects them at PREPROCESS
    // time (D-PP-DESCRIPTOR-MACRO-INJECT). Empty for non-C languages / callers
    // without a system path -> the angle-macro branch is inert.
    std::span<fs::path const>            systemDirs;
    // c9 (Phase-2): the active object-format when known. Gates the angle-include
    // macro-splice below to the SAME availability the `__has_include` callback and
    // the semantic `#include` gate use — an unavailable-on-this-format header is
    // treated like "no descriptor on the path" (left verbatim), all three agreeing.
    std::optional<ObjectFormatKind>      activeFormat;
    // D-PP-HEADER-CASE-INSENSITIVE-PE: the ACTIVE FORMAT's header-NAME case
    // rule, applied by EVERY include search this builder performs. A SEPARATE
    // input from `activeFormat` (a KIND): deriving the rule from the kind would
    // be the identity branch the agnosticism bar forbids.
    HeaderNameMatching                   headerNameMatching;
    DiagnosticReporter&                  rep;
    int                                  depth;
    std::vector<core::PathIdentity>&     includeStack;
    // D-PP-PRAGMA-RECOGNIZED-SEMANTICS: the TU's include-once registry, shared
    // by reference across the whole builder tree (like `includeStack`) because
    // include-once is a property of the TRANSLATION UNIT, not of one nesting
    // level. See `IncludeOnceRegistry` above for the identity argument.
    IncludeOnceRegistry&                 includeOnce;
    // Set TRUE when the include-nesting backstop fires (truncating the
    // splice). Shared by reference across the recursive child builders so
    // a deep-nest truncation at any level reaches `preprocess()`.
    bool&                                fatal;
    // C21 (D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE, Option 2 — supersedes the
    // C19 `seededDefines` NAME-set): a `#define NAME VALUE\n` prefix for every
    // command-line `--define` + every OBJECT-like predefined macro available on
    // the active format, shared by const-ref across EVERY child builder. The
    // include-gating pre-scan must see these VALUES so a `#if <cmdline/predefined>`
    // VALUE guard (`#if SQLITE_TEST`, `#if __STDC_VERSION__ >= 201112L`) gating a
    // quote-`#include` evaluates correctly -- it would otherwise fold to 0 -> a
    // FALSE-DEAD skip, the un-inlined header's `#define`s vanish, and the drop
    // surfaces as a spurious P0009/P9006 at the macro's use site
    // (D-PP-CONDITIONAL-INCLUDE-ORDERING lineage). `build()` prepends this as a
    // NON-EMITTED span-safe SCAN-BUFFER prefix (its `#define` lines seed
    // `localMacros` with values whose replacement-token spans slice the SAME
    // `scanBuf` sbExpand reads). This SUBSUMES C19's definedness-only seed AND
    // composes with a source `#undef` (which now erases the seeded value from
    // `localMacros`, unlike the old separate NAME set). The one-directional-
    // divergence invariant is preserved: the values EXACTLY match the
    // `<command-line>`/`<built-in>` prologues the authoritative pass sees, so the
    // pre-scan is more-live only IN LOCKSTEP (P0016 stays closed).
    std::string const& preScanDefinePrefix;
    // TF-C74: the EFFECTIVE predefined-macro list — language ⊕ target, already
    // format-resolved by `mergePredefinedMacros`. Shared by const-ref across
    // every child builder (like `preScanDefinePrefix`), so the whole include
    // tree's definedness oracle reads the SAME list the authoritative
    // `MacroExpander` seeds from. Replaces the old
    // `schema->preprocess().predefinedMacros` read + a locally re-applied
    // format filter: seed site #1 of four.
    std::span<PredefinedMacroDef const> effectivePredefines;
    // D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: sink for every system-descriptor PARENT
    // this builder (and its recursive children) SPLICES for an angle `#include <h>`
    // (or the quote->angle fallback), paired with the SYNTH-BUFFER byte offset of
    // the splice point. The splice is UN-GATED (it fires for every angle include,
    // dead branch or not -- D-PP-PRESCAN-ANGLE-MACRO-SPLICE-AUTHORITATIVE-LIVENESS);
    // `preprocess()` then DROPS any record whose offset falls in an AUTHORITATIVE
    // dead range, so the surviving set == the finish() oracle's authoritatively-live
    // `shippedLibDescriptors`. Shared by reference across every child builder (like
    // `includeStack` / `fatal`), so a header spliced deep in a quote-include chain
    // still reaches `preprocess()`. Raw (parent, offset) pairs (dups allowed);
    // `preprocess()` dead-filters, then expands the transitive `includes` closure +
    // dedups once into `PreprocessResult`. EMIT-ONLY.
    std::vector<std::pair<fs::path, ByteOffset>>& resolvedDescriptorsOut;
    // c17: a SynthBuilder-local object-like macro, tracked from LIVE-branch
    // `#define`s so a `#if FOO`/`#if FOO == 1` guard gating a quote-`#include`
    // evaluates with the macro state visible at the include point. Independent
    // of `MacroExpander`'s authoritative table (which still sees every
    // verbatim-copied `#define`); divergence is one-directional + fail-loud (a
    // false-dead skips a live include -> loud missing-symbol downstream, never a
    // silent wrong include). A FUNCTION-like `#define` records only that the name
    // is function-like (no replacement) -- an invocation of it in a guard forces
    // the CONSERVATIVE (skip) direction (FIX-3). NOTE (c17 authoritative dead-
    // regions): this pre-scan gates ONLY quote-include splicing; the dead-branch
    // `P_IllegalChar` suppression is driven by the AUTHORITATIVE `MacroExpander`
    // pass (`deadRanges()`), NOT this pre-scan -- so a guard this weaker eval
    // mis-reads only ever causes a loud include skip/resolve, never a silent
    // illegal-char drop.
    // TF-C60 (D-PP-PRESCAN-CROSS-BUFFER-MACRO-STATE): the replacement is stored
    // as TEXT, not span-tokens. The map below is SHARED across the whole builder
    // tree, and a span-token would slice the WRONG buffer in any builder other
    // than the one that recorded it — `SourceBuffer::slice` CLAMPS rather than
    // faults, so that failure mode is plausible-wrong-bytes, never a crash. The
    // text is re-tokenized into the per-evaluation product tail at expansion
    // time (`sbMintProduct`), the same FC15b mechanism `materializeSignificant`
    // uses in the authoritative pass.
    struct SbMacro {
        bool        functionLike = false;
        std::string replacementText;     // object-like body, buffer-independent
    };
    // TF-C60: SHARED BY REFERENCE across every child builder (the
    // `includeStack`/`fatal`/`resolvedDescriptorsOut` pattern), so the pre-scan's
    // macro state spans the WHOLE include tree in DOCUMENT ORDER. Before this it
    // was per-builder: a `#define` arriving via a NESTED include was invisible to
    // the parent's later `#if` (and a parent's source `#define` invisible to a
    // child's), the guard folded 0 → FALSE-DEAD → the gated quote-`#include` was
    // left verbatim → the macro pass forwarded it as inert tokens → the header
    // was SILENTLY DROPPED (sqlite os_unix.c: sqliteInt.h→os_setup.h defines
    // SQLITE_OS_UNIX, `#if SQLITE_OS_UNIX` gates `#include "os_common.h"`).
    std::unordered_map<std::string, SbMacro>& localMacros;

    PreprocessConfig const& cfg() const { return schema->preprocess(); }

    // C19/C21 (D-PP-PRESCAN-DEFINEDNESS-PARITY + -PREDEFINED-VALUE-INCLUDE-GATE):
    // the SINGLE definedness oracle for the include-gating pre-scan's
    // `#ifdef`/`#ifndef`/`#if defined()`. Before C19 the two DISAGREED -- `#ifdef`
    // saw only `localMacros`, `#if defined()` also saw `predefinedMacros` -- and
    // NEITHER saw a command-line `--define`, so a `#ifdef SQLITE_TEST`-gated
    // quote-`#include` was falsely skipped. Now BOTH consult, in one place: an
    // in-source `#define` OR a command-line `--define` OR an object-like predefined
    // (all now materialized in `localMacros` via the C21 value prefix that build()
    // prepends), OR a config predefined macro via the SHARED per-format filter the
    // authoritative MacroExpander applies (this arm keeps a FUNCTION-like predefine
    // -- excluded from the value prefix per FINDING-A -- reporting DEFINED). So the
    // pre-scan can only ever be MORE live IN LOCKSTEP with the authoritative pass --
    // never resolving a branch the real pass reads dead (the one-directional
    // divergence invariant that keeps P0016 closed). C21 IMPROVEMENT: a source
    // `#undef` of a command-line define now COMPOSES (it erases the value from
    // `localMacros` and the define is not a predefined, so this reports it
    // undefined). EDGE (pre-existing + LOUD-not-silent): a `#undef` of a PREDEFINED
    // name is still not reflected by the predefined for-loop arm -- so the pre-scan
    // may read MORE-live than the authoritative pass; the effect is at worst a
    // spurious include-resolve (loud `P_PreprocessorIncludeError`) or a benign
    // splice-then-elide, NEVER a silent mis-include.
    [[nodiscard]] bool sbNameDefined(std::string_view n) const {
        // Option 2 (C21): a command-line `--define`'s definedness now comes from
        // `localMacros` (the value prefix seeds it via the main-loop `#define`
        // handler), so the separate C19 `seededDefines` NAME set is gone. KEEP the
        // predefined arm: a FUNCTION-like predefine (EXCLUDED from the value prefix
        // per FINDING-A) must still report DEFINED here for `#if defined(NAME)` /
        // `#ifdef NAME`. TF-C74: the arm walks the pre-filtered EFFECTIVE list
        // (language ⊕ target), so mere PRESENCE means available — no local filter
        // to drift from the value prefix's predefined subset, and a per-arch
        // target predefine (`__aarch64__`) gates a quote-`#include` exactly as a
        // language one does.
        if (localMacros.find(std::string{n}) != localMacros.end()) return true;
        for (PredefinedMacroDef const& pm : effectivePredefines) {
            // ★★ D-PP-PREDEFINE-REDEFINITION-PARTITION — THIS CLAUSE CLOSES THE
            // LATENT SEAM THE PARTITION WOKE UP; IT IS NOT A TIDY-UP.
            // The EDGE the block above documents — "a `#undef` of a PREDEFINED
            // name is still not reflected by the predefined for-loop arm, so the
            // pre-scan may read MORE-live than the authoritative pass" — was
            // UNREACHABLE while the authoritative pass REFUSED every such
            // `#undef`: a directive that never takes effect cannot desynchronise
            // anything. Making `#undef` take effect makes the edge reachable for
            // EVERY row, so it is closed here, in the same change that opens it.
            // ★ The fix is to narrow the arm to exactly what it was always FOR.
            // Every OBJECT-like row — warn-class and ordinary alike — is already
            // seeded into `localMacros` by the C21 value prefix
            // (`preScanDefinePrefix`), so the branch above answers for it AND a
            // source `#undef` composes with it, exactly as for a command-line
            // `--define`. Only a FUNCTION-like row is absent from that prefix
            // (FINDING-A deliberately excludes it, so a bare `#if NAME` folds to
            // 0 in both passes), and answering for those is this arm's whole
            // C19/FINDING-A job.
            // ⚠ THE FIRST CUT OF THIS CLAUSE SKIPPED `ordinary` ROWS INSTEAD,
            // AND THAT BROKE THE INVARIANT IN THE DANGEROUS DIRECTION: a
            // function-like predefine is ordinary by construction (the loader
            // enforces it), so `#ifdef __declspec` would have read DEAD here
            // while the authoritative pass — holding it in `table_` from the
            // built-in prologue — read it LIVE. That is a quote-`#include`
            // silently not spliced, i.e. P0016 itself, not the tolerated
            // more-live direction.
            if (!pm.isFunctionLike) continue;
            if (pm.name == n) return true;
        }
        // TF-C86 (D-CSUBSET-STDARG-F001A): the language's conditional-inclusion
        // OPERATORS are DEFINED names. Kept in lockstep with the authoritative
        // `MacroExpander::isDefined` — both call the one shared predicate, so the
        // pre-scan can never read `#ifndef __has_include` live while the real pass
        // reads it dead (the one-directional divergence invariant above).
        return isConditionalInclusionOperator(n, cfg());
    }

    // Quote-include resolution for the PRE-SCAN. Historically this was a
    // PRIVATE second resolver that byte-sliced the path itself, bypassing the
    // FC15c shared funnel; it now routes every candidate through the shared
    // `resolveInDir`, so the pre-scan can never disagree with the authoritative
    // pass about which file a name denotes -- including on the CASE question
    // (D-PP-HEADER-CASE-INSENSITIVE-PE). What stays local is only the
    // `is_regular_file` filter and the resulting "keep searching the next dir"
    // behaviour, which the shared `resolveIncludePath` deliberately does not
    // have (it stops at the first existing entry, directory or not).
    //
    // ★ RETURNS THE TRI-STATE, NOT AN OPTIONAL, AND THE REASON IS A BLOCKER
    // THIS FUNCTION ONCE SHIPPED. It used to flatten `AmbiguousCase` to
    // `nullopt`, on the stated grounds that "the authoritative
    // `__has_include`, the `#embed` directive, and the import resolver
    // re-resolve the same name and fail loud there". That was TRUE of the two
    // operators and FALSE of the thing that matters most: a plain quote
    // `#include`. `import_resolver.cpp`'s directive walk does `if (ppEnabled)
    // return;` for every quote directive, and `SynthBuilder` only exists WHEN
    // preprocess runs — the two conditions are exactly complementary, so for
    // C (the only language with a preprocessor today) NOTHING re-resolves a
    // quote include. The collision fell through to the quote->angle fallback,
    // missed, and surfaced as a SUPPRESSABLE `P_PreprocessorIncludeError` with
    // the directive dropped: `--suppress` on that code turned a case collision
    // into a silently missing header.
    //
    // Handing the caller the full verdict is the fix at the level of the
    // defect. `takeFound` (the one sanctioned collapse) then forces each caller
    // to state its ambiguity policy explicitly.
    HeaderSearchResult resolveQuote(std::string_view filename,
                                    fs::path const& includingDir) const {
        // An empty include target names nothing -- fail loud (the caller
        // emits P_PreprocessorIncludeError). Only a REGULAR file is a valid
        // include target; `is_regular_file` excludes a directory (so
        // `#include ""` cannot resolve to the including dir itself, which
        // would later throw in SourceBuffer::fromFile).
        if (filename.empty()) return HeaderSearchResult::notFound();
        std::error_code ec;
        // Per candidate dir: the shared policy-aware atom, then the local
        // `is_regular_file` filter. A COLLISION is returned as-is and STOPS the
        // search -- the same rule `resolveIncludePath` states in its header
        // comment ("continuing to a later dir would make the answer depend on
        // whether the host could even represent the collision"). The earlier
        // cut of this function continued to the next dir here, quietly
        // disagreeing with the shared resolver about the same question.
        auto const tryDir = [&](fs::path const& dir) -> HeaderSearchResult {
            HeaderSearchResult r = resolveInDir(dir, filename, headerNameMatching);
            if (r.status != HeaderSearchStatus::Found) return r;
            if (!fs::is_regular_file(r.path, ec)) return HeaderSearchResult::notFound();
            return r;
        };
        // [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]: `isRootedPath`, the
        // ONE exported predicate, never a second bare `is_absolute()`. ✔MEASURED
        // -- a UNC name answers `is_absolute()` FALSE on the toolchain that
        // builds DSS, so this test sent it down the SEARCH arms instead of
        // resolving it directly. ⓘ THAT DID NOT SHOW AS A WRONG ANSWER, and the
        // reason is worth stating rather than leaving as luck: `resolveInDir`
        // recognises a rooted name itself and ignores the dir it is handed, so
        // the first arm to be tried resolved it anyway. The exposure is
        // STRUCTURAL, not observed -- with no including FILE and an empty `-I`
        // list there is no arm to try, and the loop below would return not-found
        // for a name that points somewhere real. Two tiers answering one
        // question two ways is the defect either way.
        if (isRootedPath(fs::path{filename})) return tryDir({});
        // EMPTY means "there is no including FILE", NOT "the includer has no
        // directory component" — a name like `main.c` arrives here as `.`,
        // because `includingDirectoryOf` already made that substitution
        // (D-PP-BARE-RELATIVE-MAIN-PATH-DEFEATS-THE-INCLUDER-DIRECTORY-SEARCH).
        // ⚠ Do NOT try to fix a missed self-dir search by deleting this test:
        // MEASURED, `descend` on an EMPTY base returns NotFound down both
        // matching arms (`fs::directory_iterator{fs::path{}}` fails with "Not a
        // directory"), so dropping the guard changes no answer at all — it only
        // hides where the real derivation has to happen.
        if (!includingDir.empty()) {
            HeaderSearchResult r = tryDir(includingDir);
            if (r.status != HeaderSearchStatus::NotFound) return r;
        }
        for (fs::path const& dir : includeDirs) {
            HeaderSearchResult r = tryDir(dir);
            if (r.status != HeaderSearchStatus::NotFound) return r;
        }
        return HeaderSearchResult::notFound();
    }

    void copyVerbatim(std::string const& spliced, LineMap const& localMap,
                      std::size_t from, std::size_t to,
                      std::string& out, LineMap& map) {
        if (to <= from) return;
        for (auto const& lseg : localMap.segments()) {
            if (lseg.synthEnd <= from) continue;
            if (lseg.synthStart >= to) break;
            const std::size_t a = std::max<std::size_t>(lseg.synthStart, from);
            const std::size_t b = std::min<std::size_t>(lseg.synthEnd, to);
            if (b <= a) continue;
            LineMapSegment seg;
            seg.synthStart  = static_cast<ByteOffset>(out.size());
            out.append(spliced, a, b - a);
            seg.synthEnd    = static_cast<ByteOffset>(out.size());
            seg.origin      = lseg.origin;
            seg.originStart = static_cast<ByteOffset>(
                lseg.originStart + (a - lseg.synthStart));
            map.addSegment(std::move(seg));
        }
    }

    // The outcome of resolving an include NAME to a shipped system descriptor +
    // splicing its `macros` surface (the SHARED body of the ANGLE-include path AND
    // the QUOTE→ANGLE fallback below). `NotAvailable` = no descriptor on the path,
    // or one gated unavailable on the active format (caller leaves the include
    // verbatim); `Malformed` = the descriptor exists but its macros failed to
    // decode (an error was emitted; caller should not resolve it another way);
    // `Spliced` = the descriptor exists + is available (its `#define` lines, if
    // any, were appended to `out` — zero for a typed-only descriptor).
    enum class SystemMacroSplice { NotAvailable, Malformed, Spliced };

    // Resolve `headerName` to a `<stem>.json` system descriptor and, when it
    // exists + is available on the active format, splice a synthetic `#define`
    // for each of its `macros` into `out` (D-PP-DESCRIPTOR-MACRO-INJECT). This
    // is the ONE descriptor-macro-splice used by BOTH the angle-`#include <h>`
    // arm and the quote→angle fallback, so the two never drift on availability,
    // malformed-handling, or the `#define` reconstruction. It appends ONLY the
    // `#define` lines (never the include line itself); the CALLER owns whether to
    // keep the original bytes (angle) or rewrite them to the angle form (quote
    // fallback), and owns the surrounding `copyVerbatim`. Inert (NotAvailable)
    // when there are no systemDirs.
    //
    // `reportMalformed` gates ONLY the malformed-descriptor DIAGNOSTIC (a
    // confidently-live include). The SPLICE itself is UNGATED (the
    // D-PP-PRESCAN-ANGLE-MACRO-SPLICE-AUTHORITATIVE-LIVENESS change), but the
    // `P_PreprocessorIncludeError` for a descriptor that exists-but-fails-macro-
    // decode must stay gated on confident-live: the AUTHORITATIVE pass never reads
    // the descriptor (the pre-scan is the sole emitter here), so emitting it for a
    // DEAD-branch include would break C 6.10p1 dead-branch inertness (asymmetric
    // with the still-gated quote arm). Passing `includeResolvable()` at both call
    // sites RESTORES the pre-change behavior of this diagnostic exactly: it fired
    // only on a confidently-live include before, when the whole splice was gated.
    // On a dead/uncertain branch a malformed descriptor is therefore SILENT here
    // (the branch is inert; an uncertain-but-live use still fails loud downstream as
    // the missing macro — the P0016-safe direction), never a silent MISCOMPILE.
    SystemMacroSplice spliceSystemDescriptorMacros(std::string const& headerName,
                                                   std::string& out,
                                                   bool reportMalformed,
                                                   fs::path* resolvedParentOut = nullptr) {
        if (systemDirs.empty()) return SystemMacroSplice::NotAvailable;
        // D-PP-HEADER-CASE-INSENSITIVE-PE: the SAME case policy every other
        // include search uses. A fold COLLISION reads as NotAvailable here (the
        // caller leaves the include verbatim) and is reported LOUD by the import
        // resolver, which re-resolves the surviving directive — the same
        // dead-branch-inert split the malformed-descriptor diagnostic makes.
        HeaderSearchResult const desc =
            resolveSystemDescriptor(headerName, systemDirs, headerNameMatching);
        if (desc.status != HeaderSearchStatus::Found)
            return SystemMacroSplice::NotAvailable;
        fs::path const* descPath = &desc.path;
        // If the PARENT descriptor declares this header unavailable on the active
        // object-format, treat it EXACTLY like "no descriptor on the path" — the
        // semantic gate then fails loud + `__has_include` returns false, so all
        // three descriptor consumers stay consistent (c9 MUST-FIX-3). This drives
        // the NotAvailable return (the caller leaves the include verbatim).
        if (activeFormat.has_value()
            && !ffi::shippedHeaderAvailableForFormat(*descPath, *activeFormat)) {
            return SystemMacroSplice::NotAvailable;
        }
        // D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: this angle `#include <h>` (or the
        // quote->angle fallback) resolved to a format-available descriptor whose
        // typedef surface the semantic phase will inject. Hand the resolved PARENT
        // path back to the CALL SITE, which records it together with the SYNTH-
        // BUFFER splice offset (only known there, after the verbatim copy of
        // everything up to the directive) so `preprocess()` can DROP the record if
        // the AUTHORITATIVE pass proves the include dead. NOTE: `out` here is the
        // caller's LOCAL splice buffer, NOT the synth buffer -- so the synth offset
        // cannot be read in this function. EMIT-ONLY.
        if (resolvedParentOut) *resolvedParentOut = *descPath;
        // Reconstruct one neutral macro as a `#define` line into `out`; the
        // downstream tokenizer + handleDefine build the MacroDef with the proven
        // function-like / param / redefinition machinery (an identical re-define on
        // a double-include is idempotent).
        // ── D-FFI-DESCRIPTOR-CONSTANTS-INVISIBLE-TO-THE-PREPROCESSOR ─────────
        // Render one preprocessor-visible descriptor CONSTANT into the literal
        // this language spells it with, so the synthetic `#define` below can go
        // through the SAME `handleDefine` every `macros` entry uses. The
        // descriptor hands over a NEUTRAL triple (value bits, signedness,
        // width); everything language-shaped happens here, driven by the
        // language's OWN `semantics.integerLiteralTyping` ladder.
        //
        // ★ THE SUFFIX IS SEARCHED AND VERIFIED, NEVER TABULATED. There is no
        // "unsigned means `u`" map in this file: the candidate spellings are the
        // config's own `suffixes` arrays, and the winner is the first whose
        // PHASE-4 signedness — asked of `preprocessorLiteralSignedness`, the one
        // rule the `#if` evaluator itself uses — equals the declared one. So the
        // spelled macro and the evaluator that reads it cannot disagree, and a
        // language whose ladder has different suffixes needs no change here.
        //
        // ⚠ AND THE SIGNEDNESS IS THE WHOLE POINT, not a detail. `#if -1 <
        // UINT_MAX` is FALSE in gcc AND clang (the -1 converts to uintmax_t)
        // and TRUE for any signed spelling of the same value. A decimal
        // `4294967295` would take the wrong arm just as surely as the missing
        // definition did — the defect would have moved, not closed.
        //
        // TWO FORMS, and the second exists for exactly one reason:
        //   `<digits><suffix>`      — the ordinary case.
        //   `(-<digits-1><sfx> - 1)`— the MOST NEGATIVE value of the declared
        //                             width. `-2147483648` is not a literal in
        //                             C, it is unary minus applied to
        //                             `2147483648`, which no `int` candidate
        //                             holds — so the naive spelling types
        //                             `INT_MIN` as `long` and silently changes
        //                             `sizeof(INT_MIN)` and every `_Generic` on
        //                             it. Both references' own <limits.h> spell
        //                             it compensated for this reason (gcc:
        //                             `(-0x7fffffff - 1)`), and the condition is
        //                             purely NUMERIC (value == -(2^(w-1))), so
        //                             no language knowledge enters.
        // A negative value is parenthesized so a use like `a-EOF` cannot re-parse
        // (the references parenthesize too: glibc spells `EOF` as `(-1)`).
        //
        // nullopt ⇒ no spelling this language's ladder verifies. The caller
        // REFUSES the descriptor rather than splicing a literal whose signedness
        // it could not confirm — a wrong branch in silence is the one outcome
        // this whole seam exists to prevent.
        auto const spellConstant =
            [this](ffi::ShippedPpConstant const& k) -> std::optional<std::string> {
            auto const rules = schema->semantics().integerLiteralTyping;
            if (rules.empty()) return std::nullopt;
            NumberStyle const* const ns = schema->numberStyle();

            // The first suffix spelling whose PHASE-4 signedness matches, for a
            // literal of this magnitude. Iterates the rules in CONFIG ORDER, so
            // the least-decorated spelling that works wins.
            auto const suffixFor =
                [&](std::uint64_t magnitude, bool wantUnsigned)
                -> std::optional<std::string> {
                for (auto const& r : rules) {
                    // The unsuffixed rule is spelled by the EMPTY string; every
                    // other rule contributes each of its own spellings.
                    std::vector<std::string> spellings;
                    if (r.suffixes.empty()) spellings.emplace_back();
                    else for (auto const& s : r.suffixes) spellings.push_back(s);
                    for (auto const& s : spellings) {
                        std::string const text = std::to_string(magnitude) + s;
                        auto const sgn = preprocessorLiteralSignedness(
                            text, ns, rules, magnitude);
                        if (sgn.has_value() && (*sgn != wantUnsigned)) return s;
                    }
                }
                return std::nullopt;
            };

            if (k.isUnsigned) {
                std::uint64_t const mag =
                    (k.width >= 64)
                        ? static_cast<std::uint64_t>(k.value)
                        : (static_cast<std::uint64_t>(k.value)
                           & ((std::uint64_t{1} << k.width) - 1));
                auto const sfx = suffixFor(mag, /*wantUnsigned=*/true);
                if (!sfx.has_value()) return std::nullopt;
                return std::to_string(mag) + *sfx;
            }

            std::int64_t const v = k.value;   // already sign-correct in the carrier
            if (v >= 0) {
                auto const sfx = suffixFor(static_cast<std::uint64_t>(v),
                                           /*wantUnsigned=*/false);
                if (!sfx.has_value()) return std::nullopt;
                return std::to_string(v) + *sfx;
            }
            // Negative. `-v` as a MAGNITUDE, computed in uint64 so the most
            // negative value does not overflow on its way to being spelled.
            std::uint64_t const mag =
                ~static_cast<std::uint64_t>(v) + 1u;   // two's-complement negate
            bool const mostNegative =
                (k.width <= 64) && (mag == (std::uint64_t{1} << (k.width - 1)));
            std::uint64_t const spelled = mostNegative ? (mag - 1u) : mag;
            auto const sfx = suffixFor(spelled, /*wantUnsigned=*/false);
            if (!sfx.has_value()) return std::nullopt;
            std::string body = "-" + std::to_string(spelled) + *sfx;
            if (mostNegative) body += " - 1";
            return "(" + body + ")";
        };

        auto const spliceMacro = [&out](ffi::ShippedMacro const& macro) {
            std::string def = "#define " + macro.name;
            if (macro.params.has_value()) {
                def += "(";
                bool first = true;
                for (auto const& pn : *macro.params) {
                    if (!first) def += ",";
                    def += pn;
                    first = false;
                }
                if (macro.variadic) {
                    if (!macro.params->empty()) def += ",";
                    def += "...";
                }
                def += ")";
            }
            if (!macro.replacement.empty()) {
                def += " ";
                def += macro.replacement;
            }
            def += "\n";
            out.append(def);
        };
        // D-FFI-DESCRIPTOR-INCLUDES: splice the PARENT's macros AND every
        // transitively-included sibling's, via the SHARED cycle-safe closure walker
        // (so this macro chokepoint and the import-resolver typed-surface record can
        // never disagree on the transitive set). Parent-FIRST; each descriptor is
        // independently gated by the SAME per-format availability check. The
        // PARENT's macro read drives the return value (Malformed/Spliced) exactly as
        // pre-closure. A sibling that is format-unavailable, malformed, or an
        // unresolvable `includes` entry contributes no macros and is SILENT here —
        // the import-resolver + semantic tiers read the SAME closure and own those
        // loud diagnostics (F_ShippedLibDescriptorMalformed / F_ShippedHeaderNotFound,
        // positioned on the `#include` line), mirroring the pre-closure `macroRep`
        // throwaway discipline (dead-branch inertness preserved). On elf tcl→stdio
        // this splices tcl.json's macros + stdio.json's (elf: zero — stdio's macros
        // are pe/macho stdin/stdout/stderr variants), so no elf delta; the path is
        // exercised for correctness on the other formats.
        bool parentMacrosMalformed = false;
        bool sawParent = false;
        // D-DIAG-P0016-MALFORMED-DESCRIPTOR-DETAIL-IS-BUILT-THEN-NEVER-RENDERED: the
        // loader already builds the message that says WHICH key is wrong and WHY, into
        // the throwaway reporter below. Keeping it there told the reader only that a
        // FILE is bad, leaving them to bisect the descriptor by hand — which is the
        // exact cost a fail-loud diagnostic exists to remove. This carries the detail
        // out so the P0016 text can name the offending key.
        std::string parentMacrosDetail;
        std::unordered_set<core::PathIdentity> visited;  // per-call (a splice is one root)
        ffi::forEachDescriptorInClosure(
            *descPath, systemDirs, headerNameMatching, activeFormat, visited,
            [&](fs::path const& p) {
                bool const isParent = !sawParent;
                sawParent = true;
                // Per-descriptor availability. AFTER the edge gate this can only
                // ever fire for the PARENT — the walker no longer visits a sibling
                // that is unavailable on this format (it fires `onUnavailableChild`
                // instead) and no longer descends out of an unavailable root. The
                // test is kept rather than deleted because the PARENT case is real:
                // a `#include <sys/time.h>` on pe reaches here before the semantic
                // tier refuses it, and splicing its macros in the meantime would
                // define names for a header this target does not have.
                if (activeFormat.has_value()
                    && !ffi::shippedHeaderAvailableForFormat(p, *activeFormat)) {
                    return;
                }
                DiagnosticReporter macroRep;   // throwaway — malformed surfaced downstream
                // Pass the active object-format so a per-FORMAT macro variant selects
                // the right replacement; nullopt ⇒ a variants-only macro is not injected.
                auto macros = ffi::readShippedLibMacros(p, macroRep, activeFormat);
                if (!macros) {
                    if (isParent) {
                        parentMacrosMalformed = true;
                        // FIRST diagnostic only: the loader reports the offending key
                        // once and any follow-ons are cascade. Empty stays empty, so a
                        // failure that genuinely carried no detail reads exactly as it
                        // did before rather than gaining an empty separator.
                        for (auto const& d : macroRep.all()) {
                            if (!d.actual.empty()) { parentMacrosDetail = d.actual; break; }
                        }
                    }
                    return;
                }
                for (auto const& macro : *macros) spliceMacro(macro);

                // D-FFI-DESCRIPTOR-CONSTANTS-INVISIBLE-TO-THE-PREPROCESSOR: the
                // SECOND surface this chokepoint owes the translation unit. It
                // rides the SAME closure walk, the SAME per-format availability
                // gate and the SAME throwaway-reporter discipline as the macros
                // above, so the two surfaces of one descriptor can never reach
                // the TU under different conditions.
                //
                // ⓘ NO ACTIVE TARGET IS THREADED, and the loader makes that
                // safe rather than lucky: a `preprocessorVisible` constant may
                // key its `variants` on `format` ONLY (refused at load
                // otherwise), and the format IS threaded. That refusal is what
                // stops an arch-keyed constant from being silently absent from
                // `#if` on every target.
                // ⓘ REUSES `macroRep` rather than constructing a second
                // throwaway. Not merely tidy: the reporter-enumeration pin
                // (anchor
                // D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE)
                // walks every `DiagnosticReporter` built in this file and
                // requires each to carry the operator's budget or to be
                // allowlisted with a reason — and it CAUGHT the second one. One
                // reporter for both surface reads of one descriptor is the
                // correct answer anyway: they share the same discard discipline
                // and the same owner downstream.
                auto consts = ffi::readShippedLibConstants(
                    p, macroRep, std::nullopt, activeFormat);
                if (!consts) {
                    // The macros read above already decides Malformed for the
                    // parent; a constants-only defect is surfaced by the
                    // import-resolver / semantic tier that reads the SAME
                    // descriptor with a positioned span. Silent here.
                    return;
                }
                for (auto const& k : *consts) {
                    auto const spelled = spellConstant(k);
                    if (!spelled.has_value()) {
                        // FAIL LOUD, and on the LIVE-branch condition only —
                        // the same dead-branch inertness the malformed-macro
                        // arm keeps. A constant whose spelling this language's
                        // ladder cannot verify is NOT spliced with a guess: the
                        // name then stays undefined, `#if` reads it as 0 and the
                        // semantic tier still fails loud on any real use.
                        if (reportMalformed) {
                            emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError,
                                   BufferId{}, SourceSpan::empty(0),
                                   std::string{"shipped-header descriptor constant '"}
                                       + k.name + "' has no literal spelling this "
                                         "language's integerLiteralTyping ladder "
                                         "verifies (descriptor "
                                       + core::genericSpelling(p) + ")");
                        }
                        continue;
                    }
                    out.append("#define " + k.name + " " + *spelled + "\n");
                }
            },
            [&](std::string const&, HeaderSearchResult const&) {
                /* import resolver owns F_ShippedHeaderNotFound AND
                   F_HeaderNameCaseAmbiguous — both positioned on the
                   `#include` line, both re-derived from the same closure. */ },
            [&](std::string const&, fs::path const&) {
                /* An ACTIVE edge whose child is unavailable on this format: the
                   import resolver owns the loud F_ShippedHeaderUnavailableForTarget,
                   positioned on the `#include` line and re-derived from the SAME
                   closure walk with the SAME active format. Silent HERE for the
                   reason every other verdict in this callback is silent — this tier
                   has no `#include` span and would double-report. NOT a dropped
                   fact: the resolver reports it, and invariant (i) refuses the
                   config that produces it. */ });

        if (parentMacrosMalformed) {
            // Malformed PARENT descriptor: report ONLY on a confidently-live include
            // (the `reportMalformed` note above). A dead/uncertain branch stays
            // silent — dead-branch inertness — while the include is left verbatim by
            // the caller (Malformed != Spliced) and elided by the authoritative pass.
            if (reportMalformed) {
                emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError, BufferId{},
                       SourceSpan::empty(0),
                       std::string{"shipped-header descriptor malformed (macros): "}
                           + core::genericSpelling(*descPath)
                           + (parentMacrosDetail.empty()
                                  ? std::string{}
                                  : std::string{" — "} + parentMacrosDetail));
            }
            return SystemMacroSplice::Malformed;
        }
        return SystemMacroSplice::Spliced;
    }

    // c17: record a LIVE-branch `#define` into `localMacros` for the pre-scan's
    // `#if` evaluation. `[nameP, end)` are the directive-line PPTokens AFTER the
    // `define` word. FUNCTION-like iff the function-like-open token is IMMEDIATELY
    // ADJACENT to the macro name (C 6.10.3p3: no space) -- recorded as
    // function-like with NO body (an invocation in a guard then forces the
    // conservative skip, FIX-3). OBJECT-like records the replacement tokens
    // (everything after the name, trivia-stripped) whose spans slice against the
    // scan buffer. A malformed `#define` (no name) is ignored here (the macro
    // pass reports it authoritatively). Mirrors the redefinition-tolerant table
    // write (last definition wins; the pre-scan needs no compatibility check).
    // TF-C86 (D-CSUBSET-STDARG-F001A): the SUBJECT of a `#define`/`#undef` — the
    // first significant Word on the directive line, or empty when the line is
    // malformed (the authoritative pass owns that diagnostic). Extracted so both
    // pre-scan arms can ask "is this name one the implementation owns?" using the
    // same reading of the line that `sbTrackDefine` does.
    [[nodiscard]] static std::string_view
    sbFirstNameOnLine(std::vector<PPToken> const& toks, std::size_t nameP,
                      std::size_t end) {
        std::size_t p = nameP;
        while (p < end && isTrivia(toks[p].tok)) ++p;
        if (p >= end || isNewline(toks[p].tok)
            || toks[p].tok.coreKind != CoreTokenKind::Word) {
            return {};
        }
        return toks[p].text;
    }

    void sbTrackDefine(std::vector<PPToken> const& toks, std::size_t nameP,
                       std::size_t end, SourceBuffer const& buf) {
        std::size_t p = nameP;
        while (p < end && isTrivia(toks[p].tok)) ++p;
        if (p >= end || isNewline(toks[p].tok)
            || toks[p].tok.coreKind != CoreTokenKind::Word) {
            return;   // malformed (no macro name) — macro pass fails loud
        }
        std::string const name{toks[p].text};
        std::size_t const nameIdx = p;
        ++p;
        SbMacro m;
        const auto openKind =
            schema->schemaTokens().find(cfg().functionLikeOpenToken);
        if (p < end && openKind.valid()
            && toks[p].tok.schemaKind == openKind
            && toks[p].tok.span.start() == toks[nameIdx].tok.span.end()) {
            m.functionLike = true;
            // No body needed: a function-like invocation forces the conservative
            // direction regardless of the replacement.
        } else {
            // TF-C60: record the replacement as RAW SOURCE TEXT — buffer-
            // independent (the shared map outlives this builder's scan buffer)
            // AND byte-faithful.
            //
            // ★ Do NOT join token TEXTS: a PPToken is not always a self-contained
            // spelling. The tokenizer emits a coalesced literal as an OPENER
            // token, a BODY token, and a CLOSER token
            // (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN gave the closing delimiter a
            // token of its own; it previously had none), so `'A'` is the three
            // texts `'`, `A`, `'` — joining them with a separator re-lexes as
            // something else (`' A '`), and joining them bare only happens to
            // round-trip for literals. `#define NL '\n'` guarding a conditional
            // include is ordinary C. Slicing the source range keeps every byte in
            // its original spacing, which no token-text join can promise.
            //
            // The range runs from the first significant replacement token to the
            // END of the directive line (the newline's start, or the last token's
            // end when the line is unterminated). Taking the whole line tail is
            // what makes this site IDEMPOTENT across the closer-token change: it
            // already covered the closing delimiter's bytes when they belonged to
            // no token, and covers them still now that they belong to one. Any
            // trailing comment re-lexes to trivia and is dropped by
            // `sbMintProduct`.
            std::size_t firstStart = 0;
            std::size_t lineEnd    = 0;
            bool        haveFirst  = false;
            for (std::size_t q = p; q < end; ++q) {
                if (isNewline(toks[q].tok)) { lineEnd = toks[q].tok.span.start();
                                              break; }
                if (isTrivia(toks[q].tok)) continue;
                if (!haveFirst) { firstStart = toks[q].tok.span.start();
                                  haveFirst  = true; }
                lineEnd = toks[q].tok.span.end();
            }
            if (haveFirst && lineEnd > firstStart) {
                m.replacementText =
                    std::string{buf.slice(SourceSpan::of(
                        static_cast<ByteOffset>(firstStart),
                        static_cast<ByteOffset>(lineEnd)))};
            }
        }
        localMacros[name] = std::move(m);
    }

    // TF-C60 (c′): read a token's TEXT product-awarely. A token minted by
    // `sbMintProduct` spans `[bufLen + productOffset, …)` — past the scan
    // buffer's end — so slice it from the per-eval PRODUCT string instead.
    // (`SourceBuffer::slice` would silently CLAMP an out-of-range span to
    // plausible wrong bytes, so this branch is correctness, not cosmetics.)
    static std::string_view sbTextOf(Token const& t, SourceBuffer const& buf,
                                     std::string const& product) {
        std::size_t const bufLen = buf.text().size();
        if (t.span.start() >= bufLen) {
            std::size_t const s = t.span.start() - bufLen;
            std::size_t const e = t.span.end() - bufLen;
            if (e <= product.size() && s <= e)
                return std::string_view{product}.substr(s, e - s);
            return {};
        }
        return buf.slice(t.span);
    }

    // TF-C60 (c′, the FC15b product-tail pattern): re-tokenize a shared macro's
    // replacement TEXT into the per-evaluation product string, minting tokens
    // whose spans point at `[bufLen + productBase, …)` — exactly where
    // `evaluateIfExpression` slices them from: its `IceParser::textOf` reads a
    // span at-or-past `synth.text().size()` out of `productText()` and every
    // other span out of the buffer itself, which is the rule `sbTextOf` just
    // above spells for this pass (pp_if_eval). Token KINDS come
    // from the real tokenizer, so nothing is ever re-lexed across a
    // substitution boundary (no glue hazard, no `<a/b.h>` byte corruption).
    std::vector<Token> sbMintProduct(std::string_view spelling,
                                     SourceBuffer const& buf,
                                     std::string& product) const {
        ByteOffset const productBase = static_cast<ByteOffset>(product.size());
        product.append(spelling);
        auto tiny = SourceBuffer::fromString(std::string{spelling},
                                             "<sb-product>");
        DiagnosticReporter scratch;   // pre-scan diagnostics are never user-facing
        auto ppToks = tokenizeToPP(tiny, schema, scratch);
        ByteOffset const bufLen = static_cast<ByteOffset>(buf.text().size());
        std::vector<Token> out;
        for (PPToken const& pt : ppToks) {
            if (isTrivia(pt.tok) || isNewline(pt.tok)) continue;
            if (pt.tok.coreKind == CoreTokenKind::Eof) continue;
            Token t = pt.tok;
            t.span  = SourceSpan::of(bufLen + productBase + pt.tok.span.start(),
                                     bufLen + productBase + pt.tok.span.end());
            out.push_back(t);
        }
        return out;
    }

    // c17 (reworked TF-C60): object-like macro expansion over the SHARED
    // `localMacros` for the `sbEvalIfOperand` `#if` evaluation. A bounded
    // recursive rescan (so a `#define A B` / `#define B 1` chain folds), with an
    // `active`-set self-reference guard (a `#define X X` freezes to its own
    // name, matching the full engine) + a depth backstop. FUNCTION-like names
    // are NEVER expanded here -- an invocation is already detected as
    // conservative by `sbEvalIfOperand`. Substituted tokens are MINTED into the
    // per-eval `product` string (never carried as foreign-buffer spans), so the
    // shared map stays buffer-independent.
    // ⚠ `barrier`: see `sbMakeIfOperandBarrier` just below this function for
    // what it is. It is threaded BY REFERENCE through the recursion rather
    // than re-created per frame — that is what makes it compose ACROSS the
    // replacement-list boundary. `#define D defined` + `#if D(FOO)` produces the
    // KEYWORD inside a recursive frame and its OPERAND in the parent's remaining
    // input; a per-frame barrier would see the two halves separately and protect
    // neither, and `FOO` would expand to `1` before the operator ever ran (all
    // three references answer 1 here — the operand is protected).
    // D-PP-DEFINED-VIA-MACRO-EXPANSION.
    std::vector<Token> sbExpand(std::vector<Token> const& in,
                                SourceBuffer const& buf, std::string& product,
                                std::set<std::string>& active, int depth,
                                PpIfOperandBarrier& barrier) const {
        if (depth > 32) return in;   // backstop (a pathological cycle the guard
                                     // missed never loops the host)
        std::vector<Token> outToks;
        outToks.reserve(in.size());
        for (Token const& t : in) {
            bool const isWordTok = (t.coreKind == CoreTokenKind::Word);
            std::string name;
            if (isWordTok) name = std::string{sbTextOf(t, buf, product)};
            // A PROTECTED token is copied verbatim -- never looked up in the
            // macro table, so `defined(BAR)` cannot become `defined(0)`.
            if (barrier.protects(t, isWordTok ? std::string_view{name}
                                              : std::string_view{})) {
                outToks.push_back(t);
                continue;
            }
            if (isWordTok) {
                auto it = localMacros.find(name);
                if (it != localMacros.end() && !it->second.functionLike
                    && active.find(name) == active.end()) {
                    active.insert(name);
                    std::vector<Token> minted = sbMintProduct(
                        it->second.replacementText, buf, product);
                    std::vector<Token> sub =
                        sbExpand(minted, buf, product, active, depth + 1,
                                 barrier);
                    active.erase(name);
                    for (Token const& s : sub) outToks.push_back(s);
                    continue;
                }
            }
            outToks.push_back(t);
        }
        return outToks;
    }

    // D-PP-DEFINED-VIA-MACRO-EXPANSION: the ONE place this pass builds a
    // `defined`-operand barrier (`PpIfOperandBarrier`, pp_if_eval.hpp — the same
    // class the authoritative expander and the evaluator use, so all three
    // agree on what a `defined` operand IS by construction). Keyword + paren
    // KINDS come from the schema, so a language declaring no `defined` operator
    // gets a provably inert barrier (empty keyword -> every query answers false)
    // and this pass behaves exactly as it did before the row.
    [[nodiscard]] PpIfOperandBarrier sbMakeIfOperandBarrier() const {
        return PpIfOperandBarrier{*schema};
    }

    // c17: evaluate an `#if`/`#elif` controlling expression in the SynthBuilder
    // pre-scan, to decide whether a quote-`#include` nested under it should be
    // resolved NOW (the P0016 fix). Reuses the SHARED `evaluateIfExpression`
    // (the same ICE engine + const-eval core the macro pass uses) with
    // `localMacros`-backed callbacks; diagnostics go to a SCRATCH reporter (the
    // authoritative `MacroExpander` pass re-evaluates the same `#if` and reports
    // any error -- never double-reported here). Returns the BRANCH-TAKEN
    // boolean. FIX-3 conservative fallback: if the operand invokes a
    // function-like macro OR the expression cannot be evaluated (nullopt),
    // `uncertain` is set and the result is FALSE -- the P0016-safe direction
    // (skip the include; a wrongly-skipped LIVE include fails loud downstream,
    // never a silent wrong-include). `[p, end)` are the operand tokens; `buf` is
    // the scan buffer; `includingDir` resolves a `__has_include`.
    bool sbEvalIfOperand(std::vector<Token> const& toks, std::size_t p,
                         std::size_t end, SourceBuffer const& buf,
                         fs::path const& includingDir, bool& uncertain) {
        uncertain = false;
        std::size_t last = end;
        while (last > p && isNewline(toks[last - 1])) --last;
        std::vector<Token> operand(
            toks.begin() + static_cast<std::ptrdiff_t>(p),
            toks.begin() + static_cast<std::ptrdiff_t>(last));

        // FIX-3: a function-like-macro invocation in the guard is NOT evaluated
        // by this weaker (object-like) pre-scan -- force the conservative
        // (skip) direction so a divergence can never resolve a DEAD include
        // (which would re-open P0016).
        for (Token const& t : operand) {
            if (t.coreKind != CoreTokenKind::Word) continue;
            auto it = localMacros.find(std::string{buf.slice(t.span)});
            if (it != localMacros.end() && it->second.functionLike) {
                uncertain = true;
                return false;
            }
        }

        // TF-C60 (c′): the per-EVALUATION product string. Substituted macro
        // replacements are minted here; `productCb` hands it to
        // `evaluateIfExpression`, which reads a product-region span straight out
        // of it (no concatenated buffer is built — see that function's
        // D-PERF-PP-IF-REMATERIALIZES-THE-WHOLE-SYNTH-BUFFER-PER-EVALUATION
        // note). Owned per call — it must outlive the whole evaluation, and now
        // strictly so: the ICE BORROWS these bytes rather than copying them, and
        // it slices after the expand callback returns.
        std::string sbProduct;
        // FIX-3 completion (post-expansion arm): an object-like macro can EXPAND
        // TO a function-like macro's NAME (`#define Z ENABLED(1)`), which the
        // raw-operand check above cannot see. Freezing the expansion here is NOT
        // conservative — folding an unexpandable identifier to 0 is only the safe
        // direction at EVEN polarity; `#if !Z` inverts it to TRUE and would
        // eagerly resolve an authoritatively-DEAD include, re-opening P0016 (the
        // exact hazard FIX-3 exists to forbid, and now tree-wide, since a
        // wrongly-spliced header's `#define`s pollute the SHARED map). Record the
        // uncertainty and let the caller take the conservative skip instead.
        bool sbPostExpandUncertain = false;
        PpMacroExpand expandCb =
            [this, &buf, &sbProduct,
             &sbPostExpandUncertain](std::vector<Token> const& in) {
                std::set<std::string> active;
                // D-PP-DEFINED-VIA-MACRO-EXPANSION: one barrier per evaluation,
                // threaded through the whole recursion (see `sbExpand`).
                PpIfOperandBarrier barrier = sbMakeIfOperandBarrier();
                std::vector<Token> out =
                    sbExpand(in, buf, sbProduct, active, 0, barrier);
                // D-PP-DEFINED-VIA-MACRO-EXPANSION: the uncertainty scan below
                // asks "did the expansion leave a function-like macro NAME the
                // pre-scan cannot expand?". A `defined` OPERAND is not such a
                // name — it is an operand the barrier deliberately preserved, and
                // `#if defined(F)` for a function-like `F` is a perfectly
                // decidable 1. Re-running the SAME barrier over the OUTPUT is
                // what keeps the two answers from disagreeing; a second,
                // hand-written "is this a defined operand?" test here is exactly
                // the two-paths-for-one-concept the row forbids.
                PpIfOperandBarrier scan = sbMakeIfOperandBarrier();
                for (Token const& t : out) {
                    bool const isWordTok = (t.coreKind == CoreTokenKind::Word);
                    std::string name;
                    if (isWordTok) name = std::string{sbTextOf(t, buf, sbProduct)};
                    if (scan.protects(t, isWordTok ? std::string_view{name}
                                                   : std::string_view{})) {
                        continue;
                    }
                    if (!isWordTok) continue;
                    auto it = localMacros.find(name);
                    if (it != localMacros.end() && it->second.functionLike) {
                        sbPostExpandUncertain = true;
                        return in;
                    }
                }
                return out;
            };
        PpIsDefined definedCb = [this](std::string_view n) {
            // C19/C21 (D-PP-PRESCAN-DEFINEDNESS-PARITY): unified with
            // `#ifdef`/`#ifndef` via `sbNameDefined`. A command-line `--define` is
            // seen through `localMacros` (the C21 value prefix seeds it), so
            // `#if defined(SQLITE_TEST)` and `#ifdef SQLITE_TEST` agree AND both see
            // a command-line define.
            return sbNameDefined(n);
        };
        // Resolve `__has_include` EXACTLY as the include machinery / the macro
        // pass's callback does (quote = self-dir + includeDirs; angle =
        // `<stem>.json` on systemDirs, gated by per-format availability), so the
        // pre-scan and the authoritative pass never disagree on a header's
        // existence.
        PpHasInclude hasIncludeCb =
            [this, &includingDir](std::string_view filename,
                                  bool isAngle) -> bool {
            if (isAngle) {
                // D-INCLUDE-ANGLE-SOURCE-FALLBACK + FC15c: resolve EXACTLY as the
                // angle `#include <h>` arm does, through the SHARED funnel, so
                // `__has_include(<h>)` and `#include <h>` can never disagree on
                // existence. Descriptor -> keep the existing per-format availability
                // verdict (an unavailable-on-this-format descriptor answers 0,
                // matching the arm that then leaves the include verbatim). Source ->
                // a real header on the -I path is includable (the arm textually
                // splices it) -> 1. NotFound -> 0.
                //   D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: a `__has_include(<h>)` probe
                //   is NOT an include -- it records NOTHING for the reparse-seed oracle
                //   (a live `#include <h>` seeds via its own splice; a probe with no
                //   matching include resolves nothing). Left as a pure existence answer.
                AngleIncludeResolution const ar =
                    resolveAngleInclude(filename, systemDirs, includeDirs,
                                        headerNameMatching);
                switch (ar.kind) {
                    case AngleIncludeKind::Descriptor:
                        return !(activeFormat.has_value()
                                 && !ffi::shippedHeaderAvailableForFormat(
                                        ar.path, *activeFormat));
                    case AngleIncludeKind::Source:
                        return true;
                    case AngleIncludeKind::NotFound:
                        return false;
                    case AngleIncludeKind::AmbiguousDescriptor:
                    case AngleIncludeKind::AmbiguousSource:
                        // Speculative pass — reporting here would break
                        // dead-branch inertness (C 6.10p1). Answer 0. This is
                        // safe for the OPERATOR specifically: the AUTHORITATIVE
                        // `__has_include` (MacroExpander) re-evaluates every
                        // LIVE `#if` operand through the same funnel and emits
                        // there, so an operator whose answer can change the
                        // build is never silently wrong.
                        return false;
                }
                return false;   // unreachable — every AngleIncludeKind handled above
            }
            // Explicitly silent (speculative pass, see above); the
            // AUTHORITATIVE quote `__has_include` re-resolves and emits.
            return takeFound(resolveQuote(filename, includingDir),
                             [](std::span<fs::path const>) {}).has_value();
        };
        // TF-C60 (c′): hand the per-eval product tail to the ICE — it assembles
        // `combined = synth.text() + productText()` and slices minted tokens
        // (spans at `bufLen + offset`) from the product region. An empty-view
        // callback here would make every minted span slice past the end (CLAMPED
        // to garbage) — exactly the silent-wrong-bytes class (b) exists to kill.
        PpProductText productCb =
            [&sbProduct]() { return std::string_view{sbProduct}; };
        // FC17.9(h): the pre-scan `__has_embed`, resolving against THIS
        // recursion's `includingDir` (the origin file of every token in the scan
        // buffer), so it AGREES with the authoritative per-origin callback by
        // construction. Without it, an unknown `__has_embed(` makes the eval
        // uncertain -> the conservative quote-include SKIP (a wrongly-skipped LIVE
        // include fails loud downstream, never silent), which would falsely fail
        // the legitimate `#if __has_embed("r") ... #include "impl.h"` pattern. Same
        // C23 trichotomy 0/1/2; angle form -> 0. `resolveQuote` already requires a
        // regular file, so a miss is NOT_FOUND and `file_size` gives emptiness.
        PpHasEmbed embedCb =
            [this, &includingDir](std::string_view filename, bool isAngle,
                                  SourceSpan) -> int {
            if (isAngle) return 0;
            // Explicitly silent (speculative pass); the AUTHORITATIVE
            // `__has_embed` and the `#embed` directive both re-resolve + emit.
            auto resolved = takeFound(resolveQuote(filename, includingDir),
                                      [](std::span<fs::path const>) {});
            if (!resolved) return 0;                       // NOT_FOUND
            std::error_code ec;
            auto const sz = fs::file_size(*resolved, ec);
            if (ec) return 0;
            return sz == 0 ? 2 /*EMPTY*/ : 1 /*FOUND*/;
        };

        DiagnosticReporter scratch;   // discard — re-reported by the macro pass
        auto v = evaluateIfExpression(operand, *schema, expandCb, definedCb,
                                      hasIncludeCb, buf, productCb, scratch,
                                      embedCb);
        // FIX-3 post-expansion arm (see `sbPostExpandUncertain` above): the
        // expansion produced a function-like macro name, so this weaker pre-scan
        // cannot decide the guard — take the conservative skip in BOTH polarities
        // rather than trust a frozen-identifier fold.
        if (sbPostExpandUncertain) {
            uncertain = true;
            return false;
        }
        if (!v.has_value()) {
            uncertain = true;   // malformed/unsupported -> conservative (skip)
            return false;
        }
        return *v;
    }

    // ── TF-C87 (D-PP-INCLUDE-REENTRY-GUARD-AWARE) ────────────────────────────
    // The ONE re-entry decision, called by BOTH include arms (angle-source and
    // quote) so the two can never drift into disagreeing about what a cycle is —
    // they already had two hand-copied `std::find(includeStack…)` blocks with two
    // separately-worded messages.
    //
    // Returns TRUE to PERMIT the re-entry (the caller proceeds to splice exactly
    // as for a first entry). Returns FALSE having ALREADY EMITTED the refusal —
    // and WHICH refusal is the point of this function: a user who hits a gap in
    // `detectIncludeOnceMechanism` must be able to SEE it is a detector gap
    // rather than be told "circular include" and left guessing.
    //
    // ★ THE SEPARATION IS BY DIAGNOSTIC CODE, NOT ONLY BY PROSE.
    // `P_PreprocessorIncludeError` (0x0016) is FOUR-WAY OVERLOADED — not found,
    // unreadable, THIS refusal, and the nesting-depth backstop — so rewording
    // the message alone would leave the two conditions that matter most
    // indistinguishable to every census, log filter and tool that keys on the
    // code. This refusal therefore carries its OWN code,
    // `P_PreprocessorIncludeReentryRefused` (0x0022). The depth cap in `build()`
    // deliberately KEEPS 0x0016: it is a genuine resource/structure limit and
    // makes no claim about guards. So:
    //   • permitted   → no diagnostic at all
    //   • OncePragma  → 0x0022, naming `#pragma once` as a mechanism DSS has not
    //                   built; explicitly "not a missing guard"
    //   • None        → 0x0022, saying NO include-once mechanism was FOUND,
    //                   spelling out what was looked for, and saying outright
    //                   that a guarded header reaching it is a DETECTOR GAP
    //   • depth cap   → 0x0016, "include nesting deeper than N levels"
    // The two 0x0022 arms are separated from each other by prose only, and that
    // is sufficient: both mean "this header was not re-entered", they differ
    // only in WHY, and neither is ever confusable with a depth limit.
    [[nodiscard]] bool
    permitReentry(std::shared_ptr<SourceBuffer> const& headerBuf,
                  std::string_view                     name) {
        switch (detectIncludeOnceMechanism(headerBuf, schema)) {
            case IncludeOnceMechanism::MacroGuard:
                // The guard's controlling name is already in `localMacros` from
                // the first entry (the map is SHARED across the builder tree), so
                // the re-entered body reads DEAD, resolves no nested include, and
                // the recursion converges after exactly one extra level.
                return true;
            case IncludeOnceMechanism::OncePragma:
                // ⚠ D-PP-PRAGMA-RECOGNIZED-SEMANTICS RE-WORDED THIS MESSAGE, AND
                // THE OLD WORDING IS NOW A LIE RATHER THAN MERELY STALE: it said
                // "this implementation has not built include-once dedup", which
                // stopped being true when the `includeOnceSet` landed. A message
                // that misdescribes the engine sends the user hunting the wrong
                // thing, which is the exact failure TF-C87 split this arm out to
                // prevent.
                //
                // Reaching this arm now means something NARROW and worth saying
                // precisely: the file carries an include-once pragma, DSS
                // implements that pragma, and yet the file is on the include
                // stack — so the pragma had NOT YET BEEN REACHED at the point it
                // re-entered. That is a header that includes itself ABOVE its own
                // `#pragma once`, which no include-once mechanism can terminate,
                // because the declaration comes after the recursion.
                emitPP(rep,
                       DiagnosticCode::P_PreprocessorIncludeReentryRefused,
                       BufferId{}, SourceSpan::empty(0),
                       std::string{"refusing to re-enter "} + std::string{name}
                           + ": it carries an include-once '#pragma' (its "
                             "'preprocess.pragmaEffects' row declares "
                             "'includeOnce') and DSS DOES honour that pragma — "
                             "but the file is already on the include stack, which "
                             "means the pragma had not been REACHED yet when this "
                             "re-entry happened. A header that includes itself "
                             "ABOVE its own include-once line cannot be terminated "
                             "by that line, because the declaration comes after "
                             "the recursion. This is NOT a missing guard: move the "
                             "include-once directive above the self-include");
                return false;
            case IncludeOnceMechanism::None:
                emitPP(rep,
                       DiagnosticCode::P_PreprocessorIncludeReentryRefused,
                       BufferId{}, SourceSpan::empty(0),
                       std::string{"refusing to re-enter "} + std::string{name}
                           + ": it is already on the include stack and NO "
                             "include-once mechanism was found in it — no "
                             "conditional whose controlling expression names a "
                             "macro the header then '#define's (that is what an "
                             "include guard is, in every spelling), and no "
                             "include-once '#pragma'. A self-including header "
                             "with no such mechanism is a genuine infinite "
                             "include cycle. ★ If this header IS guarded, what "
                             "you have hit is a GAP IN THE GUARD DETECTOR, not a "
                             "cycle in your code");
                return false;
        }
        return false;   // unreachable — every IncludeOnceMechanism handled above
    }

    // `pre` is the SHARED per-file pre-scan (see `preScanIncludeFile`): the
    // file's buffer, its continuation-spliced text, that text's line map, and
    // its ONE tokenize. Every INCLUDE arm passes it, so a header spliced into
    // fifty translation units is read and tokenized ONCE. The ROOT passes
    // nothing and builds its own, because a main file is unique per TU AND
    // because only the root carries the non-emitted `preScanDefinePrefix`, which
    // is a property of the INVOCATION rather than of the file — memoizing a
    // buffer that contains it would key file bytes on command-line state.
    void build(std::shared_ptr<SourceBuffer> const& source,
               std::string& out, LineMap& map,
               std::shared_ptr<PreScannedFile const> const& pre = nullptr) {
        // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN: the `"` / `>` byte constants that
        // used to sit here are GONE. Both include arms below used to re-consume
        // the closing delimiter BYTE because it belonged to no token; the closer
        // is a real token now, so the directive's end offset is read from that
        // token's span and the engine never names a delimiter byte. Strictly more
        // agnostic — a language whose string/header closer is not `"`/`>` now
        // splices correctly too.
        const char newline    = '\n';
        // TF-C87: once the backstop below has fired ANYWHERE in the builder tree
        // the whole preprocess result is already fatal, so every further splice is
        // pure waste — and not merely waste. `fatal` is shared by reference, but
        // before this it only truncated the ONE recursion that hit the cap; a
        // header that includes a recursive sibling TWICE would re-enter the cap
        // path down both arms at every level, which is exponential in `depth`.
        // Short-circuiting makes the FIRST cap hit stop the whole splice, which is
        // what "fatal" already claimed to mean.
        if (fatal) return;
        if (depth > kMaxIncludeDepth) {
            emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError, BufferId{},
                   SourceSpan::empty(0),
                   std::string{"include nesting deeper than "}
                       + std::to_string(kMaxIncludeDepth) + " levels, at "
                       + std::string{source->name()}
                       + " — the guard-aware re-entry BACKSTOP. Re-entry into a "
                         "header that carries an include guard is PERMITTED (that "
                         "is how a legal a.h -> b.h -> a.h chain terminates: the "
                         "guard makes the second expansion empty), so reaching "
                         "this depth means a header whose guard WAS detected is "
                         "still recursing — the guard is not neutralizing the "
                         "repeat include. This is NOT the refused-re-entry "
                         "diagnostic, which names the header and says no include "
                         "guard was detected in it");
            fatal = true;   // splice truncated — PP fatal
            return;
        }
        // C21 (D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE): prepend the command-
        // line/predefined `#define` VALUE prefix as a NON-EMITTED span-safe HEADER
        // of THIS build's scan buffer, so a `#if <cmdline/predefined>` VALUE guard
        // (`#if SQLITE_TEST`) evaluates with the macro's value at the include point
        // -- its replacement tokens slice the SAME `scanBuf` sbExpand reads. Two
        // LOAD-BEARING invariants keep the prefix un-emittable (so it never
        // contaminates the output and no diagnostic offset leaks):
        //   (1) it is prepended as RAW bytes with NO localMap segment (NOT through
        //       appendWithContinuationSplice), and copyVerbatim is SEGMENT-DRIVEN
        //       (it emits ONLY bytes covered by a localMap segment) -> the prefix
        //       is STRUCTURALLY un-copyable; AND
        //   (2) copiedUpTo starts at prefixLen (below), not 0.
        // The prefix's `#define` lines are consumed by the main loop's `#define`
        // handler (seeding `localMacros` with VALUES) and that handler does
        // `i=lineEndTok-1; continue;` -- it never touches copiedUpTo -- so NO
        // pre-pass is needed. appendWithContinuationSplice bases each source
        // segment at the CURRENT out.size() (== prefixLen), so the line-map stays
        // correct (source at synthStart >= prefixLen).
        // TF-C60 (Finding 7): the prefix seeds the ROOT build ONLY. `localMacros`
        // is now SHARED across the builder tree, so a child re-prepending it
        // would RE-ADD a command-line define the source had `#undef`'d before the
        // include — breaking the C21 "#undef composes" invariant in the exact
        // direction the authoritative pass resolves it (its prologue runs ONCE
        // per TU, then the #undef holds for the rest, children included). The
        // prefix's second historical role — providing sliceable SPANS for seeded
        // replacement values — is obsolete: SbMacro now stores replacement TEXT.
        // ── The four pure per-file results, from the memo when this is an
        // INCLUDE (`pre`), built here when it is the ROOT. Held by const
        // reference either way, so everything below reads one shape: the memo
        // entry is immutable and shared across threads, and nothing in this
        // function has ever written to any of the four
        // (D-PERF-PP-EVERY-INCLUDE-RE-READS-AND-RE-TOKENIZES-THE-SAME-HEADER).
        std::string                   ownSpliced;
        LineMap                       ownMap;
        std::shared_ptr<SourceBuffer> ownScanBuf;
        std::vector<PPToken>          ownToks;
        std::size_t                   prefixLen = 0;
        // THROWAWAY: every diagnostic this pre-scan raises — the tokenizer's and
        // the conditional handlers' below — is discarded here and re-raised by
        // the AUTHORITATIVE pass over the same bytes. Function-scoped because the
        // conditional-directive walk further down reports into it too.
        DiagnosticReporter scratch;
        if (!pre) {
            ownSpliced = (depth == 0) ? preScanDefinePrefix : std::string{};
            prefixLen  = ownSpliced.size();
            appendWithContinuationSplice(source->text(), source, 0, ownSpliced,
                                         ownMap);
            ownScanBuf = SourceBuffer::fromString(ownSpliced,
                                                  std::string{source->name()});
            ownToks = tokenizeToPP(ownScanBuf, schema, scratch);
        }
        std::string const&  spliced  = pre ? pre->spliced  : ownSpliced;
        LineMap const&      localMap = pre ? pre->localMap : ownMap;
        std::shared_ptr<SourceBuffer> const& scanBuf =
            pre ? pre->scanBuf : ownScanBuf;
        std::vector<PPToken> const& toks = pre ? pre->toks : ownToks;

        const auto hashKind =
            schema->schemaTokens().find(cfg().directiveIntroToken);
        const auto quoteKind =
            schema->schemaTokens().find(cfg().quoteIncludeToken);
        const auto angleKind =
            schema->schemaTokens().find(cfg().angleIncludeToken);

        // C21: start PAST the non-emitted value prefix (load-bearing invariant 2).
        std::size_t copiedUpTo = prefixLen;
        // D-PP-BARE-RELATIVE-MAIN-PATH-DEFEATS-THE-INCLUDER-DIRECTORY-SEARCH:
        // the SHARED derivation, never a local `parent_path()`. A bare
        // `dss --compile main.c` gives this buffer the name `main.c`, whose
        // parent is the EMPTY path, and an empty includer dir turns off the
        // self-dir arm of `resolveQuote` below.
        fs::path const includingDir = includingDirectoryOf(source->name());

        auto isHash = [&](Token const& t) {
            return hashKind.valid() && t.schemaKind == hashKind;
        };

        // c17 (D-PP-CONDITIONAL-INCLUDE-ORDERING): the SynthBuilder-local
        // conditional state, used ONLY to gate quote-`#include` splicing (the
        // P0016 fix). `sbCondStack` mirrors the macro pass's stack so a
        // quote-`#include` is resolved ONLY when its enclosing branches are all
        // live; `sbFrameUncertain[k]` marks a frame whose controlling expression
        // this weaker pre-scan could not confidently evaluate (a function-like-
        // macro invocation / malformed expr -- FIX-3), in which case NO include in
        // that whole group resolves (the conservative skip; a wrongly-skipped LIVE
        // include fails loud downstream, never a silent miscompile). The
        // dead-branch `P_IllegalChar` suppression is NOT driven from here -- it
        // uses the AUTHORITATIVE `MacroExpander::deadRanges()` (full macro table),
        // so a guard this pre-scan mis-reads can only ever cause a loud include
        // skip/resolve, never a silent illegal-char drop.
        std::vector<CondFrame> sbCondStack;
        std::vector<char>      sbFrameUncertain;   // parallel to sbCondStack
        bool                   sbEvalUncertain = false;   // last eval's verdict
        auto anyUncertain = [&]() {
            for (char u : sbFrameUncertain) if (u) return true;
            return false;
        };
        // Include resolution is gated on CONFIDENT-LIVE (active AND nothing
        // uncertain in the enclosing chain).
        auto includeResolvable = [&]() {
            return sbStackActive(sbCondStack) && !anyUncertain();
        };

        // The token index just past the newline that ends the directive line
        // starting at token `start` (or toks.size() at EOF). Mirrors
        // `MacroExpander::lineEnd`.
        auto sbLineEndTok = [&](std::size_t start) {
            std::size_t e = start;
            while (e < toks.size() && !isNewline(toks[e].tok)) ++e;
            if (e < toks.size()) ++e;   // include the newline token
            return e;
        };

        // ★ A `#` is a DIRECTIVE INTRO only when it is FIRST on its line
        // (C 6.10p1). The authoritative `MacroExpander` loop has always
        // enforced this (`isHash(in[i]) && firstOnLine(in, i)`); this pre-scan
        // did NOT, so ANY `#` anywhere — in ordinary program text, not just in
        // a directive payload — was read as a directive of its own. The
        // unhandled-directive line skip below fixed the PAYLOAD half of that;
        // this fixes the other half, which the skip cannot reach because the
        // stray `#` is not on a directive line at all. WITNESS (bare `#` in
        // ORDINARY TEXT, inside a dead group):
        //     #if 0
        //     x # endif y          <- the prose `#endif` POPS the frame
        //     #include "missing.h" <- now looks live -> eagerly resolved
        //     #endif
        // produced a spurious `P0016` on a header the authoritative pass never
        // looks at. Mirrors `firstOnLine` exactly, over the pre-scan's own
        // token wrapper (`toks[p].tok`).
        auto sbFirstOnLine = [&](std::size_t idx) {
            for (std::size_t p = idx; p-- > 0;) {
                if (isNewline(toks[p].tok)) return true;
                if (!isTrivia(toks[p].tok)) return false;
            }
            return true;   // start of buffer
        };
        for (std::size_t i = 0; i < toks.size(); ++i) {
            if (!isHash(toks[i].tok) || !sbFirstOnLine(i)) continue;
            std::size_t j = i + 1;
            while (j < toks.size() && isTrivia(toks[j].tok)) ++j;
            if (j >= toks.size()) break;
            std::string_view const dirWord = toks[j].text;

            // ── c17: the conditional-compilation directives drive `sbCondStack`
            // (so the include gate agrees with the authoritative macro pass on
            // which quote-includes are live), then skip the rest of the directive
            // LINE (its operand must not be re-scanned as include syntax). The
            // handler logic is the SHARED `sbHandle*` free functions (the macro
            // pass drives the same ones). The C23 `#elifdef`/`#elifndef` words are
            // OPTIONAL (guarded `.empty()` so a stripped/pre-C23 config is inert,
            // mirroring the handleDirective + pragma opt-in). ──
            bool const isCond = (dirWord == cfg().ifDirective)
                || (dirWord == cfg().ifdefDirective)
                || (dirWord == cfg().ifndefDirective)
                || (dirWord == cfg().elifDirective)
                || (!cfg().elifdefDirective.empty()
                    && dirWord == cfg().elifdefDirective)
                || (!cfg().elifndefDirective.empty()
                    && dirWord == cfg().elifndefDirective)
                || (dirWord == cfg().elseDirective)
                || (dirWord == cfg().endifDirective);
            if (isCond) {
                std::size_t const lineEndTok = sbLineEndTok(i);
                // The operand starts just after the directive word (token j+1),
                // bounded by the directive line's last token.
                auto textOfTok =
                    [&](Token const& t) { return scanBuf->slice(t.span); };
                auto isDefinedTok = [&](std::string_view n) {
                    // C19 (D-PP-PRESCAN-DEFINEDNESS-PARITY): was localMacros-ONLY,
                    // which diverged from `#if defined()` (definedCb) and missed
                    // command-line `--define`s + predefined macros -> a
                    // `#ifdef SQLITE_TEST`-gated quote-`#include` was falsely
                    // skipped and its `#define`s dropped. Now the SAME oracle.
                    return sbNameDefined(n);
                };
                // The `#if`/`#elif` value comes from the local pre-scan eval;
                // `sbEvalUncertain` reports whether it was confident.
                auto evalExprTok = [&](std::vector<Token> const& in,
                                       std::size_t p, std::size_t end) {
                    bool unc = false;
                    bool const taken =
                        sbEvalIfOperand(in, p, end, *scanBuf, includingDir, unc);
                    sbEvalUncertain = unc;
                    return taken;
                };
                // Flatten the directive-line PPTokens to a `Token` vector so the
                // shared handlers (which take `vector<Token>`) can read the
                // operand. The directive word is at index (j - i) in this slice.
                std::vector<Token> lineToks;
                lineToks.reserve(lineEndTok - i);
                for (std::size_t q = i; q < lineEndTok; ++q) {
                    lineToks.push_back(toks[q].tok);
                }
                std::size_t const wordIdx = j - i;       // directive word
                std::size_t const opStart = wordIdx + 1; // operand start
                std::size_t const opEnd   = lineToks.size();
                sbEvalUncertain = false;
                if (dirWord == cfg().ifDirective
                    || dirWord == cfg().ifdefDirective
                    || dirWord == cfg().ifndefDirective) {
                    SbIfKind const kind =
                        (dirWord == cfg().ifDirective)    ? SbIfKind::Expr
                        : (dirWord == cfg().ifdefDirective) ? SbIfKind::Ifdef
                                                            : SbIfKind::Ifndef;
                    bool const enclosingActive = sbStackActive(sbCondStack);
                    sbHandleIf(sbCondStack, lineToks, opStart, opEnd, kind,
                               textOfTok, isDefinedTok, evalExprTok, scratch,
                               scanBuf->id());
                    // The new frame is uncertain iff its (evaluated) controlling
                    // expression was uncertain. The eval ran only when the
                    // enclosing context was active AND it is the `#if`(Expr) form
                    // (definedness is always confident).
                    bool const evaluated = enclosingActive
                        && kind == SbIfKind::Expr;
                    sbFrameUncertain.push_back(
                        (evaluated && sbEvalUncertain) ? 1 : 0);
                } else if (dirWord == cfg().elifDirective
                           || (!cfg().elifdefDirective.empty()
                               && dirWord == cfg().elifdefDirective)
                           || (!cfg().elifndefDirective.empty()
                               && dirWord == cfg().elifndefDirective)) {
                    // C23 `#elifdef`/`#elifndef` route through the SAME
                    // `sbHandleElif` with the DIRECT definedness path (kind); a
                    // plain `#elif` stays Expr. The word match is guarded by
                    // `.empty()` so a stripped/pre-C23 config never treats the
                    // word as a conditional here.
                    SbIfKind const kind =
                        (!cfg().elifdefDirective.empty()
                         && dirWord == cfg().elifdefDirective)
                            ? SbIfKind::Ifdef
                        : (!cfg().elifndefDirective.empty()
                           && dirWord == cfg().elifndefDirective)
                            ? SbIfKind::Ifndef
                            : SbIfKind::Expr;
                    SourceSpan const at =
                        (opStart <= opEnd && opStart > 0
                             ? lineToks[opStart - 1].span
                             : SourceSpan::empty(0));
                    bool const beforeActive = sbStackActive(sbCondStack);
                    sbHandleElif(sbCondStack, lineToks, opStart, opEnd, kind, at,
                                 textOfTok, isDefinedTok, evalExprTok, scratch,
                                 scanBuf->id());
                    // OR uncertainty into the TOP frame (a group is uncertain if
                    // ANY of its branches' guards was uncertain) -- ONLY for the
                    // Expr form. The defined path (elifdef/elifndef) is always
                    // CONFIDENT (a name is defined or not), so it never marks the
                    // frame uncertain (mirrors the #ifdef/#ifndef open). The elif
                    // operand is evaluated only when the group may still take.
                    if (kind == SbIfKind::Expr && !sbFrameUncertain.empty()
                        && beforeActive && sbEvalUncertain) {
                        sbFrameUncertain.back() = 1;
                    }
                } else if (dirWord == cfg().elseDirective) {
                    sbHandleElse(sbCondStack, toks[j].tok.span, scratch,
                                 scanBuf->id());
                } else {   // endif
                    sbHandleEndif(sbCondStack, toks[j].tok.span, scratch,
                                  scanBuf->id());
                    if (!sbFrameUncertain.empty()) sbFrameUncertain.pop_back();
                }
                i = lineEndTok - 1;   // skip the operand (++i lands past the line)
                continue;
            }

            // ── c17: track STACK-LIVE `#define` / `#undef` into `localMacros` so
            // a later `#if FOO` guard in THIS file evaluates with the macro state
            // at the include point. Dead-branch defines are ignored (C 6.10p1).
            // Gated on `sbStackActive` (the line executes), NOT `includeResolvable`
            // -- macro state never resolves an include itself (the include gate is
            // separate), so tracking a define under an uncertain group's live
            // branch only improves later-guard accuracy, never causes a wrong
            // include. Then skip the directive line (the replacement list must not
            // be scanned as include syntax). ──
            // TF-C86 (D-CSUBSET-STDARG-F001A): `sbFirstNameOnLine` reads the
            // directive's SUBJECT so both arms can refuse a conditional-inclusion
            // OPERATOR name — the pre-scan must reach the SAME macro state the
            // authoritative pass will, and that pass REFUSES the define/undef
            // (P_PreprocessorOperatorNameNotDefinable). Recording it here would
            // put a function-like `__has_include` in `localMacros`, which is
            // precisely what tripped FIX-3's uncertainty bail and produced the
            // F001A cascade. The DIAGNOSTIC stays with the authoritative pass
            // alone (this pre-scan re-walks the same lines; emitting here would
            // double-report one root cause).
            if (sbStackActive(sbCondStack) && dirWord == cfg().defineDirective) {
                std::size_t const lineEndTok = sbLineEndTok(i);
                auto const subject = sbFirstNameOnLine(toks, j + 1, lineEndTok);
                if (!isConditionalInclusionOperator(subject, cfg())) {
                    sbTrackDefine(toks, j + 1, lineEndTok, *scanBuf);
                }
                i = lineEndTok - 1;
                continue;
            }
            if (sbStackActive(sbCondStack) && dirWord == cfg().undefDirective) {
                std::size_t const lineEndTok = sbLineEndTok(i);
                auto const subject = sbFirstNameOnLine(toks, j + 1, lineEndTok);
                if (!subject.empty()
                    && !isConditionalInclusionOperator(subject, cfg())) {
                    localMacros.erase(std::string{subject});
                }
                i = lineEndTok - 1;
                continue;
            }

            // ── D-PP-PRAGMA-RECOGNIZED-SEMANTICS: `#pragma once` FIRES HERE ───
            //
            // A REACHED pragma whose registry row declares `includeOnce` records
            // THIS FILE's identity in the TU-wide `includeOnceSet`; both include
            // arms below consult that set and SKIP a repeat splice.
            //
            // ★★★ WHY THE EFFECT IS REALIZED IN THE PRE-SCAN AND NOT AT
            // `applyPragma`. `applyPragma` runs in the AUTHORITATIVE
            // `MacroExpander` pass, which walks a buffer this builder has ALREADY
            // spliced — by the time the pragma is seen there, the second copy of
            // the header's text is in the buffer and the decision is spent.
            // Include-once is an INCLUDE-MACHINERY effect, so the only place it
            // can be honoured is the machinery that performs the splice.
            //
            // ★★★ GATED ON `includeResolvable()` — THE CONSERVATIVE ORACLE, AND
            // THE DIRECTION IS THE WHOLE ARGUMENT. ✔MEASURED 2026-08-29, all
            // four references AGREE that a `#pragma once` buried in a NOT-TAKEN
            // `#if 0` does NOT fire (WSL gcc 13.3.0, WSL clang 18.1.3, mingw-w64
            // gcc 13.2.0, MSVC 19.51.36252 — every one re-splices and fails with
            // a redefinition). So recording must be reachability-aware, and the
            // two error directions are NOT symmetric:
            //   • recorded-but-actually-dead  → a later include is SKIPPED →
            //     TEXT SILENTLY VANISHES. The class the bar most abhors.
            //   • not-recorded-but-live       → the header is spliced twice →
            //     a LOUD redefinition. Recoverable, and self-announcing.
            // `includeResolvable()` (stack-active AND nothing uncertain in the
            // enclosing chain) is the same gate that already decides whether an
            // `#include` is resolved at all, so this adds no new liveness
            // judgement — it reuses the one the splice itself is already trusting.
            //
            // ⚠ THIS IS A REAL DIVERGENCE FROM `detectIncludeOnceMechanism`, AND
            // DELIBERATELY SO. That detector is documented as "deliberately NOT
            // gated on the conditional stack", which was CORRECT for its own
            // reader: it gates whether re-entry is PERMITTED, where
            // over-recognition merely splices again and the real conditional
            // logic decides. Here over-recognition DROPS content, so the safe
            // direction is inverted and the gate must be too. Same word, opposite
            // risk — which is why this arm does not call that function.
            //
            // Deliberately NO `continue`: the line falls through to the existing
            // flow exactly as before, so this arm ADDS a record and changes
            // nothing else about how a `#pragma` line is pre-scanned.
            if (!cfg().pragmaDirective.empty()
                && dirWord == cfg().pragmaDirective && includeResolvable()
                && !includeStack.empty()) {
                std::size_t const         lineEndTok = sbLineEndTok(i);
                std::vector<std::string>  pragmaWords;
                for (std::size_t p = j + 1; p < lineEndTok; ++p) {
                    if (isTrivia(toks[p].tok) || isNewline(toks[p].tok)) continue;
                    if (toks[p].tok.coreKind == CoreTokenKind::Eof) continue;
                    pragmaWords.emplace_back(toks[p].text);
                }
                auto const pm = matchPragmaEffect(cfg(), pragmaWords);
                if (pm.has_value() && pm->effect == PragmaEffect::IncludeOnce) {
                    // `includeStack.back()` IS this builder's own file: the
                    // parent pushes the resolved identity before constructing the
                    // child, and `preprocess()` pushes the main source for the
                    // root. One rule, no special case for depth 0.
                    includeOnce.record(includeStack.back(),
                                       includeStack.back().path());
                }
            }

            // ── D-CPP-ERROR-WARNING (F2): skip a DIAGNOSTIC directive's line. Its
            // operand is PROSE, and prose routinely contains the very words this
            // pre-scan hunts for — `#error you must #define FOO first`, `#error see
            // #include "config.h"`. Two pre-scan properties make that dangerous:
            // the fall-through below does NOT skip the rest of the line (so the
            // loop keeps stepping token-by-token INTO the message), and this
            // loop's hash test (`isHash`, above) has NO `firstOnLine` guard —
            // unlike the authoritative `MacroExpander` loop — so an embedded `#`
            // inside the prose is read as a directive of its own. The first shape
            // would harvest a phantom macro into `localMacros`; the second would
            // reach the include arm and eagerly resolve/splice a header the
            // program never asked for. Skipping the line is what EVERY other
            // directive this pre-scan recognises already does, and for the same
            // reason (see the conditional and define/undef arms above).
            //
            // Deliberately NOT gated on `sbStackActive`, unlike the define/undef
            // arms: those MUTATE `localMacros`, so they must only run on a live
            // branch. This one mutates nothing — it only advances `i` past bytes
            // that are not include/define syntax in either case — so skipping is
            // correct live OR dead. (The DIAGNOSTIC itself is not emitted here at
            // all; the authoritative `MacroExpander` pass owns that, below its
            // dead-branch gate.)
            //
            // ★ STATED AS THE GENERAL INVARIANT, not a per-directive list
            // (D-PP-PRESCAN-UNHANDLED-DIRECTIVE-LINE-SKIP). Everything this
            // pre-scan actually PROCESSES has already been dispatched above:
            // the `#if`-family, `#define`, `#undef`. So by this point `dirWord`
            // is either `include` (handled just below) or a directive this pass
            // does not interpret at all — `#error`/`#warning`, `#pragma`,
            // `#line`, `#embed`, or a word no config declares. For EVERY one of
            // those the rest of the line is opaque payload, never include or
            // define syntax, so stepping into it token-by-token can only
            // misfire. Enumerating the words was the original shape and it
            // aged badly: `#error`/`#warning` were added here only after the
            // TF-C70 repro showed a prose `#define` being harvested and a prose
            // `#include` being eagerly resolved (with the spliced-out include
            // even corrupting the emitted message) — while `#pragma`/`#line`
            // silently kept the same exposure. Skipping the line for ANY
            // unhandled directive closes the class once, and closes it for
            // directives not yet invented.
            if (dirWord != cfg().includeDirective) {
                i = sbLineEndTok(i) - 1;   // ++i lands just past the line
                continue;
            }
            std::size_t k = j + 1;
            while (k < toks.size() && isTrivia(toks[k].tok)) ++k;
            if (k >= toks.size()) continue;
            const bool isQuote =
                quoteKind.valid() && toks[k].tok.schemaKind == quoteKind;
            if (!isQuote) {
                // D-PP-PRESCAN-ANGLE-MACRO-SPLICE-AUTHORITATIVE-LIVENESS (Option B):
                // the angle shipped-macro splice is NOT gated on the pre-scan's
                // (weaker) conditional verdict -- UNLIKE the quote-include INLINE
                // below, which MUST stay gated on confident-live (P0016) because it
                // EAGERLY resolves a file. The two differ fundamentally: the angle
                // splice only EMITS synthetic `#define` lines INSIDE the include's
                // conditional region (right before the KEPT `#include <h>` line), so
                // the AUTHORITATIVE MacroExpander pass -- which has the full, correct
                // macro table and ELIDES dead-branch `#define`s (handleDirective
                // returns early on !stackActive()) -- is the proper arbiter of the
                // injected defines' liveness. Gating on the pre-scan here was a BUG:
                // the pre-scan is BLIND to a quote-included header's `#define`s, so it
                // CONFIDENTLY folds a `#if <macro-defined-in-a-quote-include>` to 0
                // (an undefined identifier -> 0, C 6.10.1p4) and mis-marks the branch
                // dead -- suppressing the splice on VALID, authoritatively-LIVE code
                // (the errno / test_syscall `#if SQLITE_OS_UNIX` -> `#include <errno.h>`
                // S0001). One-directional-safe (P0016 preserved): a TRULY-dead branch
                // still elides the injected defines in the authoritative pass (the
                // final token stream is byte-identical), so a dead-branch shipped
                // include never leaks a live macro -- witnessed by the negative pin +
                // the preprocessor_dead_branch_include example.
                // D-PP-DESCRIPTOR-MACRO-INJECT: an ANGLE `#include <h>` whose
                // shipped descriptor declares a `macros` surface — splice a
                // synthetic `#define` for each into the synth buffer BEFORE the
                // include line, so the macro is in the table for the rest of the
                // source AND its replacement tokens carry spans valid in the final
                // buffer (they point into the synthText prefix, like ordinary
                // source). The include line itself is LEFT in place (copiedUpTo
                // stays at dirStart) so the post-parse import resolver still
                // injects the typed surfaces (symbols/constants/typedefs). Inert
                // when the language declares no angle token or there are no
                // systemDirs.
                if (!angleKind.valid() || toks[k].tok.schemaKind != angleKind) {
                    continue;
                }
                // The angle BODY is the coalesced token immediately after the
                // opener (mirrors the quote-body extraction below).
                const std::size_t aBody = k + 1;
                if (aBody >= toks.size() || isTrivia(toks[aBody].tok)
                    || isNewline(toks[aBody].tok)
                    || toks[aBody].tok.span.start() != toks[k].tok.span.end()) {
                    continue;  // malformed/empty angle include — leave verbatim
                }
                std::string const angleName{toks[aBody].text};
                if (angleName.empty()) continue;

                // D-INCLUDE-ANGLE-SOURCE-FALLBACK: the SHARED angle funnel —
                // descriptor FIRST (the DSS neutral `<stem>.json` model), else a
                // REAL source header on the -I includeDirs, else a miss. The SAME
                // funnel answers `__has_include(<h>)`, so `#include`/`__has_include`
                // never disagree on existence (FC15c). Descriptor / NotFound both
                // flow the UNCHANGED macro-splice-or-verbatim path below (Descriptor
                // keeps the line for the import resolver's typed-surface injection;
                // NotFound is left verbatim -> the resolver emits the hard
                // F_ShippedHeaderNotFound). Only Source is new.
                // D-PP-HEADER-CASE-INSENSITIVE-PE, corrected at H2. The two
                // ambiguous arms are NOT interchangeable here:
                //   * AmbiguousDescriptor — the import resolver re-resolves the
                //     `<stem>.json` half and reports it; staying silent here
                //     avoids a double-report (the malformed-descriptor
                //     discipline directly above).
                //   * AmbiguousSource — NOTHING downstream re-resolves the `-I`
                //     source half (the import resolver's angle arm calls
                //     `resolveSystemDescriptor` alone), so leaving it verbatim
                //     surfaced it as `F_ShippedHeaderNotFound`: loud, but naming
                //     the wrong defect, while the diagnostic that exists to list
                //     the colliding paths never fired. THIS tier reports it.
                // Gated on `includeResolvable()` for the same C 6.10p1
                // dead-branch-inertness reason as every other emit in this pass.
                AngleIncludeResolution const angleRes =
                    resolveAngleInclude(angleName, systemDirs, includeDirs,
                                        headerNameMatching);
                if (angleRes.kind == AngleIncludeKind::AmbiguousSource
                    && includeResolvable()) {
                    reportHeaderCaseAmbiguity(rep, BufferId{},
                                              SourceSpan::empty(0), angleName,
                                              angleRes.ambiguousCandidates);
                    continue;   // left verbatim; the macro pass elides it
                }

                // SOURCE fallback: a no-descriptor angle header that IS a real source
                // file on the -I path — splice it TEXTUALLY and DROP the directive,
                // byte-for-byte like the quote-on-disk arm below, so an angle
                // `<sqlite3ext.h>` behaves exactly like the quote `"sqlite3ext.h"`
                // that the 39 clean ext TUs already use (its macros + typedefs inline
                // into THIS TU; a CrossTreeRef would carry neither). GATED on
                // includeResolvable() for the SAME P0016 reason as the quote arm: an
                // EAGER file inline in a dead/uncertain branch is forbidden — left
                // verbatim, the authoritative pass elides a truly-dead one, and a
                // wrongly-skipped live one fails loud downstream (never a silent
                // miscompile). A dead/uncertain-branch source include therefore falls
                // through to the descriptor arm which (no descriptor) leaves it
                // verbatim. Mirrors the quote arm's includeStack + circular guard.
                if (angleRes.kind == AngleIncludeKind::Source
                    && includeResolvable()) {
                    const ByteOffset dStart = toks[i].tok.span.start();
                    // Directive end: PAST the angle body's `>` closer, read off the
                    // closer's own token (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN).
                    // `toks[aBody]` spans only the header name — its span stops
                    // BEFORE the `>`, so cutting there would leave the delimiter in
                    // the output. Mirrors the quote arm below.
                    const ByteOffset dirEnd =
                        literalEndPastCloser(*schema, toks, aBody);
                    auto const canon = core::PathIdentity::of(angleRes.path);
                    // TF-C87: the buffer is loaded BEFORE the stack test now,
                    // because the re-entry decision reads the header's own text.
                    // Order-independent in practice: a header already ON the stack
                    // was readable when it was pushed.
                    // The load now goes through the shared per-file pre-scan, so
                    // the read + splice + tokenize happen once per FILE rather
                    // than once per OCCURRENCE; a null return means exactly what
                    // a null `SourceBuffer::fromFile` meant.
                    auto headerPre = preScanIncludeFile(angleRes.path, schema);
                    if (!headerPre) {
                        emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError,
                               BufferId{}, SourceSpan::empty(0),
                               std::string{"system include unreadable: "} + angleName);
                        continue;
                    }
                    auto const& headerBuf = headerPre->source;
                    // D-PP-PRAGMA-RECOGNIZED-SEMANTICS: this file's `#pragma once`
                    // already fired in this TU — SKIP the repeat splice. Checked
                    // BEFORE the include-stack test because include-once is the
                    // stronger statement: a file that says "once" needs no cycle
                    // adjudication, and answering here keeps the two mechanisms
                    // from having to agree about a case only one of them owns.
                    if (includeOnce.alreadySpliced(canon, angleRes.path)) {
                        copyVerbatim(spliced, localMap, copiedUpTo, dStart, out,
                                     map);
                        copiedUpTo = dirEnd;   // DROP the directive, splice nothing
                        continue;
                    }
                    if (std::find(includeStack.begin(), includeStack.end(), canon)
                            != includeStack.end()
                        && !permitReentry(headerBuf, angleName)) {
                        continue;   // permitReentry emitted the refusal
                    }
                    copyVerbatim(spliced, localMap, copiedUpTo, dStart, out, map);
                    includeStack.push_back(canon);
                    SynthBuilder child{schema, includeDirs, systemDirs, activeFormat,
                                       headerNameMatching,
                                       rep, depth + 1, includeStack,
                                       includeOnce, fatal,
                                       preScanDefinePrefix, effectivePredefines,
                                       resolvedDescriptorsOut, localMacros};
                    child.build(headerBuf, out, map, headerPre);
                    includeStack.pop_back();
                    out.push_back(newline);
                    copiedUpTo = dirEnd;   // DROP the directive — content is inlined
                    continue;
                }

                // Splice the descriptor's macros into a LOCAL buffer FIRST (so a
                // NotAvailable outcome touches neither `out` nor the line-map). On
                // NotAvailable (no descriptor / unavailable) OR Malformed (already
                // emitted) leave the include fully verbatim. Otherwise copy up to
                // the directive, emit the `#define` lines, then KEEP the include
                // line in place (copiedUpTo = dStart) so the post-parse import
                // resolver still injects the typed surfaces (a typed-only
                // descriptor splices zero macros but the line is still kept).
                std::string defs;
                fs::path    splicedParent;
                // Splice UNGATED (the authoritative pass arbitrates liveness), but
                // report a malformed descriptor ONLY on a confidently-live include
                // (`includeResolvable()`) — restoring the pre-change dead-branch
                // inertness of that diagnostic.
                if (spliceSystemDescriptorMacros(angleName, defs,
                                                 /*reportMalformed=*/includeResolvable(),
                                                 &splicedParent)
                    != SystemMacroSplice::Spliced) {
                    continue;
                }
                const ByteOffset dStart = toks[i].tok.span.start();
                copyVerbatim(spliced, localMap, copiedUpTo, dStart, out, map);
                // D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: record the resolved parent +
                // the SYNTH-BUFFER offset of the splice point (`out.size()` now, after
                // the verbatim copy up to the directive, before the spliced `#define`s
                // land -> the offset sits INSIDE the include's conditional region).
                // `preprocess()` drops this record if the offset lies in an
                // AUTHORITATIVE dead range, so the seed set matches the finish() oracle
                // EXACTLY (never a superset). EMIT-ONLY.
                resolvedDescriptorsOut.push_back(
                    {splicedParent, static_cast<ByteOffset>(out.size())});
                out.append(defs);
                copiedUpTo = dStart;  // KEEP the include line — final copyVerbatim copies it
                continue;
            }

            // ★ c17 (D-PP-CONDITIONAL-INCLUDE-ORDERING, the P0016 fix): a
            // quote-`#include` is resolved/spliced ONLY when its enclosing
            // conditional branches are ALL confidently live. In a dead branch
            // (`#if 0 #include "x.h" #endif`) or an uncertain group (a guard this
            // pre-scan could not evaluate, FIX-3), the include is LEFT VERBATIM
            // for the macro pass to elide -- so a dead-branch include never
            // resolves x.h (and a missing dead-branch include never errors). The
            // wrongly-skipped LIVE include (only on the conservative uncertain
            // edge) fails loud downstream as a missing symbol -- never silent.
            if (!includeResolvable()) continue;

            // The quote opener (StringStart) consumed only the opening quote;
            // the coalesced string BODY is the very next token, whose text is
            // the raw path bytes between the quotes. The tokenizer emits a
            // coalesced body with core kind Operator (its schema kind is the
            // string-body kind), so we key on POSITION (next token) rather
            // than core kind. An empty body (`#include ""`) leaves filename
            // empty -> resolveQuote fails loud below.
            const std::size_t bodyIdx = k + 1;
            std::string filename;
            const ByteOffset dirStart = toks[i].tok.span.start();
            ByteOffset dirEnd = toks[k].tok.span.end();
            if (bodyIdx < toks.size() && !isTrivia(toks[bodyIdx].tok)
                && !isNewline(toks[bodyIdx].tok)
                && toks[bodyIdx].tok.span.start() == toks[k].tok.span.end()) {
                filename = std::string{toks[bodyIdx].text};
                // PAST the closing quote, read off the closer's own token
                // (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN). The body's span still
                // stops BEFORE the `"` — deliberately, so `filename` stays
                // quote-free — so this must not cut at the body end.
                dirEnd = literalEndPastCloser(*schema, toks, bodyIdx);
            }

            // ★ H1 (D-PP-HEADER-CASE-INSENSITIVE-PE): THIS is the site whose
            // silence was a blocker. We are past `includeResolvable()`, so the
            // include is CONFIDENTLY LIVE and reporting here is dead-branch
            // inert by construction — and this tier is the ONLY one that ever
            // sees a quote directive once the preprocessor is enabled (the
            // import resolver returns early on every quote include when
            // `ppEnabled`). A collision therefore MUST fail loud right here,
            // with the unsuppressable `F_HeaderNameCaseAmbiguous`, and must NOT
            // be allowed to fall through to the quote->angle fallback and out
            // the suppressable `P_PreprocessorIncludeError` exit below.
            bool quoteCollision = false;
            auto resolved = takeFound(
                resolveQuote(filename, includingDir),
                [&](std::span<fs::path const> candidates) {
                    quoteCollision = true;
                    reportHeaderCaseAmbiguity(rep, BufferId{},
                                              SourceSpan::empty(0), filename,
                                              candidates);
                });
            if (quoteCollision) {
                // Sole reporter (the TF-C60 Finding-5 discipline): DROP the
                // directive so the macro pass's unresolved-live-quote-include
                // fail-loud does not re-report one root cause twice. Dropping is
                // safe HERE precisely because the diagnostic we just emitted is
                // unsuppressable — which is exactly what was not true before.
                copyVerbatim(spliced, localMap, copiedUpTo, dirStart, out, map);
                copiedUpTo = dirEnd;
                continue;
            }
            if (!resolved) {
                // QUOTE→ANGLE fallback (C 6.10.2p3, §B iii): a `#include "h"` NOT
                // found on disk (self-dir + includeDirs, checked FIRST above so a
                // real on-disk quote header is NEVER shadowed) RETRIES on the
                // system path — the SAME `<stem>.json` shipped-descriptor lookup
                // the angle form uses. This lets a source that quote-includes a
                // system header (sqlite's `#include "windows.h"`) resolve the
                // shipped descriptor. On a hit: splice its macros AND REWRITE the
                // directive to the angle form in the output, so the post-parse
                // import resolver (which owns typed-surface injection and, with the
                // preprocessor enabled, ONLY sees angle includes) injects the
                // types/structs/constants. A quote header that is neither on disk
                // NOR a shipped descriptor stays the same hard error as before.
                if (!filename.empty()) {
                    std::string defs;
                    fs::path    splicedParent;
                    // This fallback is reached only PAST the quote arm's
                    // `includeResolvable()` gate (below), so the include is
                    // confidently-live here — report a malformed descriptor loud.
                    SystemMacroSplice const sr =
                        spliceSystemDescriptorMacros(filename, defs,
                                                     /*reportMalformed=*/includeResolvable(),
                                                     &splicedParent);
                    if (sr != SystemMacroSplice::NotAvailable) {
                        // Malformed already emitted its own error; on Spliced the
                        // macros are in `defs`. Either way rewrite quote→angle:
                        // emit the descriptor macros, then a synthetic
                        // `#include <filename>` in place of the quote bytes.
                        copyVerbatim(spliced, localMap, copiedUpTo, dirStart,
                                     out, map);
                        // D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: record the resolved
                        // parent + the synth-buffer splice offset (`out.size()` after
                        // the verbatim copy, before the spliced `#define`s). This
                        // fallback is past `includeResolvable()`, so the branch is
                        // pre-scan-live; the dead-range filter in `preprocess()` stays
                        // the authority. EMIT-ONLY.
                        resolvedDescriptorsOut.push_back(
                            {splicedParent, static_cast<ByteOffset>(out.size())});
                        out.append(defs);
                        out.append("#include <");
                        out.append(filename);
                        out.append(">\n");
                        copiedUpTo = dirEnd;   // drop the original quote bytes
                        continue;
                    }
                }
                emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError,
                       BufferId{}, SourceSpan::empty(0),
                       std::string{"quote include not found: "} + filename);
                // TF-C60 (Finding 5): DROP the directive so the macro pass's
                // unresolved-live-quote-include fail-loud does not re-report the
                // same problem a second time. This error is the sole reporter.
                copyVerbatim(spliced, localMap, copiedUpTo, dirStart, out, map);
                copiedUpTo = dirEnd;
                continue;
            }
            auto const canon = core::PathIdentity::of(*resolved);
            // TF-C87: loaded BEFORE the stack test (the re-entry decision reads
            // the header's own text) — see the angle arm's note. Routed through
            // the shared per-file pre-scan for the same reason it is there.
            auto headerPre = preScanIncludeFile(*resolved, schema);
            std::shared_ptr<SourceBuffer> const headerBuf =
                headerPre ? headerPre->source : nullptr;
            if (!headerBuf) {
                emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError,
                       BufferId{}, SourceSpan::empty(0),
                       std::string{"quote include unreadable: "} + filename);
                copyVerbatim(spliced, localMap, copiedUpTo, dirStart, out, map);
                copiedUpTo = dirEnd;   // Finding 5: sole reporter, drop the line
                continue;
            }
            // D-PP-PRAGMA-RECOGNIZED-SEMANTICS: this file's `#pragma once` already
            // fired in this TU — SKIP the repeat splice. Same placement and same
            // reasoning as the angle arm above (before the include-stack test).
            if (includeOnce.alreadySpliced(canon, *resolved)) {
                copyVerbatim(spliced, localMap, copiedUpTo, dirStart, out, map);
                copiedUpTo = dirEnd;   // DROP the directive, splice nothing
                continue;
            }
            if (std::find(includeStack.begin(), includeStack.end(), canon)
                    != includeStack.end()
                && !permitReentry(headerBuf, filename)) {
                // permitReentry emitted the refusal; drop the directive line so
                // the macro pass's unresolved-live-quote-include fail-loud does
                // not re-report one root cause twice (TF-C60 Finding 5).
                copyVerbatim(spliced, localMap, copiedUpTo, dirStart, out, map);
                copiedUpTo = dirEnd;
                continue;
            }

            copyVerbatim(spliced, localMap, copiedUpTo, dirStart, out, map);

            includeStack.push_back(canon);
            SynthBuilder child{schema, includeDirs, systemDirs, activeFormat,
                               headerNameMatching, rep,
                               depth + 1, includeStack, includeOnce, fatal,
                               preScanDefinePrefix,
                               effectivePredefines, resolvedDescriptorsOut,
                               localMacros};
            child.build(headerBuf, out, map, headerPre);
            includeStack.pop_back();

            out.push_back(newline);
            copiedUpTo = dirEnd;
        }
        copyVerbatim(spliced, localMap, copiedUpTo, spliced.size(), out, map);
    }
};

// FC13 cycle 4 (D-PP-MACRO-HIDESET-PRECISE): the precise per-token HIDE SET
// (Prosser's realization of C 6.10.3.4). `Token` is a 16B trivially-copyable POD
// that cannot itself carry a set, so the macro-expansion WORKING set is a wrapper
// pairing each token with the set of macro names that must NOT be expanded for
// THAT token. The hide set PERSISTS through the produced stream (per-token),
// rather than being scoped to the recursion that produced it -- which is exactly
// what lets a function-like name and its `(` re-pair when they become adjacent
// only ACROSS the boundary between a just-expanded replacement and the
// surrounding parent stream (`A(F)(3)`, `NAME(4)`).
//
// Representation: a `shared_ptr<const set<string>>`. The set is IMMUTABLE once
// built, so copies are a refcount bump (every token in one replacement shares
// the SAME set object); union/intersect allocate a fresh set only when they
// actually change the contents. A null pointer is the canonical EMPTY hide set
// (the common case -- most tokens are hidden by nothing), so the parser-bound
// body lifts in with `nullptr` and pays no allocation. `std::set` (ordered)
// makes the hot intersection/union a linear merge.
using HideSet = std::shared_ptr<std::set<std::string> const>;

struct ExpToken {
    Token   tok;
    HideSet hide;  // null == empty
    // FC15b (predefined macros; C 6.10.8.1): the INVOCATION offset that an
    // offset-derived predefined macro (`__LINE__`/`__FILE__`) resolves against.
    // C 6.10.8.1: `__LINE__` is the line of the macro's INVOCATION (the current
    // SOURCE line), NOT the `#define` site. An ORIGINAL body token carries its
    // OWN synth offset here; a macro's spliced replacement token INHERITS the
    // invoking token's `invOffset` (propagated DOWN through every nested/chained
    // expansion). So a `__LINE__` that arrives via `#define WARN __LINE__` resolves
    // against the WARN INVOCATION line, not the define line. The BARE case
    // (`int x = __LINE__;`) is the degenerate instance: the `__LINE__` token's own
    // `invOffset` IS its source position. Defaults to the token's own span start
    // when a token is lifted from a plain `Token` (`fromToken` below).
    ByteOffset invOffset = 0;
    // FC15 paste residuals (D-PP-PASTE-PLACEMARKER, C 6.10.3.3p2): a PLACEMARKER
    // is a sentinel for an EMPTY `##`-operand argument (`#define J(a,b) a##b`
    // called `J(x,)` -> `x`). It is NOT a real token: `tok` is left
    // default-constructed and is never inspected (every placemarker is consumed
    // by `collapsePastes`'s placemarker-aware branches, then any survivor is
    // dropped before the result leaves `substitute`). The default `false` makes
    // every existing ExpToken construction a non-placemarker -- zero regression.
    bool placemarker = false;
    // ── [[D-PP-PASTE-PRODUCT-IS-RE-READ-AS-THE-PASTE-OPERATOR]] ────────────
    //
    // TRUE for a token this pass MINTED by pasting. C 6.10.3.3p3 lets the
    // resulting token take part in further macro replacement, and NOT in further
    // `##` evaluation: the operator's own pass is done once its operands are
    // concatenated. Without the flag `collapsePastes` rescans from the product
    // and, when the product IS the `##` spelling (`CAT(#, #)` -> `##`), reads it
    // as its own operator and fails loud on a replacement list that is perfectly
    // well formed. ✔MEASURED: gcc 13.3.0 and clang 18.1.3 both form the `##`
    // token and pass it through; DSS answered
    // "'##' must not appear at the start of a macro replacement list".
    //
    // ⚠ Rescanning from the product is still REQUIRED and is not what changed:
    // `a##b##c` chains because the NEXT `##` is a replacement-list token, which
    // this flag leaves alone.
    bool pasteProduct = false;
    // ★★ "White space separated this token from the previous one IN THE CONSTRUCT
    // IT CAME FROM" -- the ONE owner of pp-token adjacency, and the ONLY thing
    // C23 6.10.5.2p3 (`#`) is allowed to consult. ALWAYS maintained.
    //
    // ★★ WHY A CARRIED BIT AND NEVER A SPAN COMPARISON. 6.10.5.2p3 turns each
    // occurrence of white space BETWEEN the stringizing argument's tokens into one
    // space and inserts NONE where the source had none. ✔MEASURED
    // (clang-18/clang-19/gcc-13/cl 19.51 unanimous): with X=p,
    //   `#define SZ(X, ...) #__VA_OPT__(X+X)`   -> "p+p"
    //   `#define SZ(...)    #__VA_OPT__(a + b)` -> "a + b"
    //   `#define SZ(...)    #__VA_OPT__(a+b)`   -> "a+b"
    // Adjacency belongs to the CONSTRUCT, and once tokens have been substituted it
    // is no longer recoverable from their spans: in `X+X` the substituted `p` comes
    // from the CALL SITE while the `+` comes from the `#define` line, so the two are
    // never byte-adjacent in ANY buffer and a span comparison wrongly says "p + p".
    // D-PP-STRINGIZE-EXPANDED-ARG-SLICES-WRONG-BYTES is what happens when a span is
    // trusted anyway: the slice ran off the argument entirely and shipped the
    // `#define` line -- or the rest of the file -- inside the string literal, with
    // no diagnostic.
    //
    // ★ WRITTEN AT EXACTLY TWO PLACES, WHERE ADJACENCY IS GENUINELY KNOWN, AND
    // ONLY EVER COPIED THEREAFTER:
    //   (1) `liftRun` -- an ORIGINAL source run, whose own TRIVIA tokens are still
    //       interleaved, so the answer is read off the white space itself (exact,
    //       and it needs no contiguity assumption at all). A comment is trivia, so
    //       it correctly counts as one space (✔MEASURED `S(a/*x*/b)` -> "a b").
    //   (2) `substituteRange`'s `spacedHere` -- a REPLACEMENT-LIST token, where
    //       trivia was dropped at `#define` time but the list IS one contiguous run
    //       of the define line, so the span gap is exact.
    // Everything downstream (`stampArg`, `collapsePastes`, the splice sites)
    // PROPAGATES the bit; nothing recomputes it. Defaults to `false`.
    bool spacedBefore = false;
};

// Lift a plain (directive-stripped, original) body token into the expansion
// working set: empty hide set + its OWN synth offset as the invocation anchor
// (FC15b). Every original source token thus seeds `__LINE__`/`__FILE__` against
// its real position; macro splices later inherit the invoking token's anchor.
inline ExpToken fromToken(Token const& t) {
    return ExpToken{t, nullptr, t.span.start()};
}

// Lift a whole ORIGINAL source run into the expansion working set, stamping each
// significant token's `spacedBefore` (site (1) of the two writers named on that
// field). This is THE production point for source-origin adjacency: the run still
// carries its own TRIVIA tokens, so "was there white space before this token" is
// read off the white space itself rather than inferred from byte positions -- exact,
// and correct for a comment too (phase 3 makes a comment one space).
//
// ★ TRIVIA IS KEPT IN THE RUN, not filtered: `trimArgTrivia`, `nextSignificant` and
// `hasSignificantToken` all read it, and 6.10.5.1p7's emptiness question is asked of
// PREPROCESSING tokens, which white space is not. Only significant tokens get a bit;
// a trivia token's own bit is never read.
//
// The leading token of a run has no predecessor, so its bit stays `false` -- which is
// also 6.10.5.2p3's answer (leading white space of a stringizing argument is deleted).
[[nodiscard]] inline std::vector<ExpToken> liftRun(std::vector<Token> const& toks) {
    std::vector<ExpToken> work;
    work.reserve(toks.size());
    bool pendingSpace = false;
    for (Token const& t : toks) {
        ExpToken e = fromToken(t);
        if (isTrivia(t) || isNewline(t)) {
            pendingSpace = true;
        } else {
            e.spacedBefore = pendingSpace;
            pendingSpace   = false;
        }
        work.push_back(e);
    }
    return work;
}

// A macro's replacement run REPLACES the invoking NAME token, so the run's first
// significant token inherits that name's own `spacedBefore` -- otherwise the
// spacing that separated the invocation from its predecessor is lost.
// ✔MEASURED (all four oracles): `#define PLAIN(a,b) g(a, b)` under a two-level
// stringize gives "a g(1, 2)" for `a PLAIN(1,2)` and "a+g(1, 2)" for `a+PLAIN(1,2)`
// -- the `g` carries the spacing of the `PLAIN` it replaced, not the define line's.
inline void inheritLeadingSpacing(std::vector<ExpToken>& run, bool spaced) {
    for (ExpToken& e : run) {
        if (e.placemarker) continue;
        if (isTrivia(e.tok) || isNewline(e.tok)) continue;
        e.spacedBefore = spaced;
        return;
    }
}

// M is hidden for this token iff M is a member of its hide set.
inline bool hideContains(HideSet const& hs, std::string const& name) {
    return hs && hs->count(name) != 0;
}

// hs ∪ {name}. Returns a set that CONTAINS every element of `hs` plus `name`.
// Reuses `hs` unchanged when `name` is already present (no allocation).
inline HideSet hideAdd(HideSet const& hs, std::string const& name) {
    if (hideContains(hs, name)) return hs;
    auto next = std::make_shared<std::set<std::string>>();
    if (hs) *next = *hs;
    next->insert(name);
    return next;
}

// a ∩ b. The Prosser function-like rule intersects the macro NAME's hide set
// with the CLOSING paren's before adding the invoked macro. An empty operand
// yields the empty set (null). Reuses an operand verbatim when it is a subset of
// the other (a very common case: the close paren came from the same stream as
// the name, so one hide set contains the other) to avoid an allocation.
inline HideSet hideIntersect(HideSet const& a, HideSet const& b) {
    if (!a || a->empty() || !b || b->empty()) return nullptr;
    auto out = std::make_shared<std::set<std::string>>();
    std::set_intersection(a->begin(), a->end(), b->begin(), b->end(),
                          std::inserter(*out, out->end()));
    if (out->empty()) return nullptr;
    return out;
}

struct MacroDef {
    std::vector<Token>       replacement;
    std::string              text;
    // D-PP-REDEFINITION-IGNORES-WHITESPACE-PRESENCE: C 6.10.3p2 makes two
    // replacement lists identical only when their white-space separation agrees
    // in PRESENCE -- the AMOUNT is immaterial ("4 + 38" vs "4  +  38" is the
    // SAME list; "40+2" vs "40 + 2" is NOT). `text` joins tokens with a single
    // space UNCONDITIONALLY, which keeps token boundaries unambiguous but erases
    // exactly that distinction, so this parallel bitmap carries it: one '0'/'1'
    // per token, '1' iff white space (or a comment, which C counts as white
    // space) separated it from the PREVIOUS token in the source. Kept as a
    // SEPARATE field rather than encoded into `text` with a sentinel byte
    // because no byte is safe -- a string-literal token's text is its source
    // SPELLING, and a source file may legally contain any byte inside one.
    // ✔MEASURED: without this, `#define M 40+2` then `#define M 40 + 2` passed
    // SILENTLY here while both reference compilers diagnose it.
    std::string              spacing;
    // FC13 cycle 2 (D-PP-FUNCTION-LIKE-MACRO): function-like macros. An
    // object-like macro keeps isFunctionLike=false + an empty params; a
    // function-like one records its parameter NAMES in declared order (used to
    // map a call's argument list onto the replacement). C11/C23 6.10.3p4: a
    // redefinition must agree on BOTH the kind (object vs function-like) AND
    // the parameter spelling, not just the replacement text.
    bool                     isFunctionLike = false;
    std::vector<std::string> params;
    // FC13 cycle 3 (D-PP-VARIADIC-MACRO): a VARIADIC function-like macro
    // (`#define V(a, ...) ...` or `#define V(...) ...`). `params` still holds the
    // NAMED parameters declared BEFORE the `...`; `isVariadic` marks that a
    // trailing `...` catch-all is present, so an invocation binds the first
    // `params.size()` arguments to the named params and gathers the REST (which
    // may be empty, C23) into the configured `variadicArgsName` (`__VA_ARGS__`)
    // catch-all. A non-variadic macro keeps isVariadic=false; `__VA_ARGS__` in
    // its replacement is then a constraint violation (fail loud).
    bool                     isVariadic = false;
    // [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: the synth offset of this macro's
    // NAME token on its `#define` line — the DEFINITION SITE a macro-expansion
    // note points at ("expanded from macro 'X'"). ✔MEASURED, clang 18.1.3 emits
    // exactly that note for a diagnostic whose subject is a `#`/`##` product, and
    // gcc 13.3.0 the sibling "in definition of macro 'X'". A synth PREFIX offset
    // (the `#define` line is physically present in the spliced text), so it
    // resolves through the ordinary segment scan onto the real file.
    //
    // `hasDefinitionSite` is false only for a def built by a caller that has no
    // directive line to point at (the test constructions), never on the
    // `handleDefine` path — so absence means "no location", not "offset zero".
    ByteOffset               definitionSite = 0;
    bool                     hasDefinitionSite = false;
};

// C 6.10.9p1 DE-STRINGIZE, for the `_Pragma("...")` operand: delete the leading
// `L` when present, delete the outer quotes, then replace each `\"` with `"` and
// each `\\` with `\`. Returns nullopt when `lexeme` is not a quoted string at all
// (the caller fails loud — a `_Pragma` operand that is not a single string
// literal is a constraint violation, not something to guess at).
//
// ★ The escape pass is NOT the general string-literal decoder: 6.10.9p1 names
// exactly these two replacements, and running a full decoder here would silently
// turn a `\n` that a pragma legitimately contains into a newline byte.
//
// `body` is the literal's BODY text — delimiters already excluded by the
// tokenizer (an `L` prefix rides on the opener token), so the delete-the-quotes
// half of 6.10.9p1 has already happened by construction and only the escape
// replacement is left. Total, not fallible: the CALLER decides whether the
// operand was a string literal at all, by TOKEN KIND rather than by inspecting
// bytes for a `"`.
[[nodiscard]] inline std::string destringizePragma(std::string_view body) {
    std::string out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()
            && (body[i + 1] == '"' || body[i + 1] == '\\')) {
            out.push_back(body[i + 1]);
            ++i;
            continue;
        }
        out.push_back(body[i]);
    }
    return out;
}

class MacroExpander {
public:
    // `synth` is the PREFIX buffer (the synthesized text BEFORE any `#`/`##`
    // product is appended); `prefixLen` is its byte length. FC15a (A2): a `#`/`##`
    // product's spelling is accumulated into `productText_` and a product token's
    // span points at `[prefixLen + offsetInProductText, ...)`. After `run()`,
    // `preprocess()` appends `productText()` to the synth text and freezes the
    // FINAL buffer (prefix unchanged + products in the appended tail) -- so every
    // token (original prefix span OR product tail span) slices to its real text in
    // that ONE final buffer, exactly what the parser parses.
    MacroExpander(std::shared_ptr<SourceBuffer> synth,
                  std::shared_ptr<GrammarSchema const> schema,
                  DiagnosticReporter& rep, ByteOffset prefixLen,
                  // [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: NON-const, because the
                  // expander is the PRODUCER of the product tail's coordinates.
                  // The map owns "what is this synth offset"; minting a byte
                  // that belongs to no file is an answer only this pass knows,
                  // so it is written into the same map rather than into a
                  // side-table a consumer would have to re-pair with it.
                  LineMap* lineMap,
                  // D-PP-HEADER-CASE-INSENSITIVE-PE: REQUIRED, and ahead of
                  // the defaulted block for the same reason `preprocess()`
                  // states — a silent fallback here would put the choice back
                  // out of sight at the one site that matters most.
                  HeaderNameMatching headerNameMatching,
                  std::span<fs::path const> includeDirs = {},
                  std::span<fs::path const> systemDirs = {},
                  std::optional<ObjectFormatKind> activeFormat = {},
                  fs::path includingDir = {},
                  // TF-C74: the EFFECTIVE predefined list (language ⊕ target,
                  // already format-resolved). Defaults to {} so the ~dozen
                  // test/helper constructions compile unchanged; `preprocess()`
                  // always passes the merged list.
                  std::span<PredefinedMacroDef const> effectivePredefines = {})
        : synth_(std::move(synth)), schema_(std::move(schema)), rep_(rep),
          prefixLen_(prefixLen), lineMap_(lineMap),
          includeDirs_(includeDirs), systemDirs_(systemDirs),
          activeFormat_(activeFormat),
          headerNameMatching_(headerNameMatching),
          includingDir_(std::move(includingDir)) {
        // FC15b (predefined macros; C 6.10.8): seed the predefined-macro map
        // (name -> def) from config. An identifier that is NOT a `#define`d
        // macro but IS a predefined name materializes its configured value (see
        // `expand`). EMPTY when the language declares none (toy / tsql), so the
        // engine is a strict identity pass for `__LINE__` &c.
        // TF-C74: iterate the EFFECTIVE list (language ⊕ target), whose entries
        // are ALREADY format-resolved by `mergePredefinedMacros` — the per-format
        // availability filter that used to be re-applied here (and at the three
        // other seed sites) now runs exactly once, so this authoritative
        // `predefined_` seed CANNOT drift from the pre-scan's value prefix +
        // sbNameDefined. Presence in the list IS availability.
        for (PredefinedMacroDef const& pm : effectivePredefines) {
            // c105 (D-PP-FUNCTION-LIKE-PREDEFINE): a FUNCTION-LIKE predefine is
            // NOT seeded here — it lowers to a `#define name(params) value`
            // line in the "<built-in>" prologue (see `preprocess()`), making it
            // an ORDINARY macro (the directive handler owns its param parsing +
            // expansion). Seeding it here too would make the prologue #define
            // trip the C 6.10.8.1 predefined-collision guard against itself.
            if (pm.isFunctionLike) continue;
            // D-PP-PREDEFINE-REDEFINITION-PARTITION: an `ordinary` row is NOT
            // seeded here either — it lowers to a "<built-in>" `#define` beside
            // the function-like ones, so `table_` owns it and the ordinary
            // directive handler gives `#undef`, the 6.10.5p2 redefinition
            // policy and `#ifdef` agreement with no new machinery. `predefined_`
            // is now exactly the WARN set, which is also exactly the set
            // `handleDefine`/`handleUndef` diagnose — the two facts are one fact.
            if (!predefinedNameIsDiagnosedOnChange(pm.programRedefinition)) {
                continue;
            }
            predefined_.emplace(pm.name, pm);
        }
        // FC15b: compute the translation DATE/TIME spellings ONCE (C 6.10.8.1 --
        // both stay CONSTANT through a translation unit). `__DATE__` is
        // `"Mmm dd yyyy"` with a SPACE-padded day (e.g. `"Jun  4 2026"`);
        // `__TIME__` is `"hh:mm:ss"`. Computed only when at least one date/time
        // macro is declared (no `std::time` call for a language that needs none).
        // TF-C74: scan the EFFECTIVE list too, not `cfg().predefinedMacros` — a
        // target-declared date/time macro (or a format-gated language one that
        // the filter just dropped) must drive this decision from the SAME list
        // `predefined_` was seeded from, or the expander would look up a
        // date/time spelling it never computed.
        bool needDate = false, needTime = false;
        for (PredefinedMacroDef const& pm : effectivePredefines) {
            if (pm.kind == PredefinedMacroKind::Date) needDate = true;
            if (pm.kind == PredefinedMacroKind::Time) needTime = true;
        }
        if (needDate || needTime) computeDateTime(needDate, needTime);
        hashKind_  = schema_->schemaTokens().find(cfg().directiveIntroToken);
        // FC15a: the STRINGIZE (`#`) and TOKEN-PASTE (`##`) operator kinds are
        // CONFIG lexemes (agnosticism), resolved from `stringizeToken` /
        // `pasteToken`. OPTIONAL: an empty config field leaves the kind
        // InvalidSchemaToken, so `isStringize`/`isPaste` (`.valid()`-guarded)
        // never fire and the engine is a strict FC14 (no `#`/`##` handling).
        stringizeKind_ = schema_->schemaTokens().find(cfg().stringizeToken);
        pasteKind_     = schema_->schemaTokens().find(cfg().pasteToken);
        // The function-like-macro `(` is a CONFIG lexeme, not a hard-coded
        // name (agnosticism: a language whose paren token is named differently
        // would otherwise mis-classify `#define F(x)` as object-like). The
        // loader REQUIRES + validates `functionLikeOpenToken`, so this resolves
        // for any opt-in language; the `.valid()` guard at the use site is
        // defense-in-depth.
        parenOpen_ = schema_->schemaTokens().find(cfg().functionLikeOpenToken);
        // The function-like-macro `)` is ALSO a CONFIG lexeme (FC13 cycle 2):
        // it terminates the parameter-list parse AND balance-tracks a call's
        // argument list. `)` lexes as core `Punctuation` (not a distinct core
        // kind), so it must be resolved from config like the opener -- never
        // hard-coded. The loader REQUIRES + validates `functionLikeCloseToken`,
        // so this resolves for any opt-in language; the `.valid()` guard at the
        // use site is defense-in-depth.
        parenClose_ = schema_->schemaTokens().find(cfg().functionLikeCloseToken);
        // The argument/parameter SEPARATOR (C's `,`) is likewise a CONFIG lexeme
        // (FC13 cycle 2): a `,` lexes as core `Punctuation`, so it is resolved
        // from `functionLikeArgSeparatorToken` rather than a hard-coded name.
        argSep_ = schema_->schemaTokens().find(cfg().functionLikeArgSeparatorToken);
        // The VARIADIC marker (C's `...`) is ALSO a CONFIG lexeme (FC13 cycle 2
        // review fold): `parseParamList` detects `#define V(...)` by this token
        // KIND, never by the hard-coded `...` lexeme -- a second
        // preprocess-opting language whose variadic marker is spelled differently
        // is then parsed correctly. OPTIONAL: when the language declares none,
        // `variadicMarkerToken` is empty and this stays InvalidSchemaToken, so
        // the `.valid()` guard never treats any token as the marker.
        variadicMarker_ =
            schema_->schemaTokens().find(cfg().variadicMarkerToken);
        // FC17.9(h): the QUOTE-include opener kind (`"` -> StringStart) that
        // `handleEmbed` matches to find the `#embed "resource"` filename, and the
        // ANGLE opener kind (the REUSED `hasIncludeAngleOpenToken` = LtOp) so a
        // `#embed <resource>` gets the specific angle-form deferral message. Both
        // are CONFIG kinds (agnosticism), OPTIONAL: an empty field leaves the kind
        // InvalidSchemaToken and the `.valid()` guards never fire (a language that
        // declares no `#embed` never reaches handleEmbed anyway).
        quoteIncludeKind_ =
            schema_->schemaTokens().find(cfg().quoteIncludeToken);
        embedAngleOpenKind_ =
            schema_->schemaTokens().find(cfg().hasIncludeAngleOpenToken);
    }

    // TRUE iff a fatal nesting-backstop truncated the expansion.
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

    // D-PERF-1 effectiveness metric: the total FRONT-splice token-moves the macro
    // pass performed -- `(consumed + produced)` summed across every `spliceOver`.
    // With the front-consumed deque each splice's PHYSICAL cost IS exactly this
    // (pop_front the consumed run + push_front the replacement), so the metric is
    // the pass's real splice work and stays LINEAR in the token count; a strict
    // test pins it <= k*N. (The old mid-vector erase+insert did the SAME logical
    // token-moves but ALSO shifted the whole tail per call -- the O(n^2) wall-clock
    // the deque removes.) Surfaced onto `PreprocessResult::macroTokenMoves`.
    [[nodiscard]] std::size_t tokenMoves() const noexcept { return tokenMoves_; }

    // c17 (D-PP-CONDITIONAL-INCLUDE-ORDERING, authoritative dead-regions): the
    // dead conditional byte ranges this pass recorded (see `deadRanges_`).
    // `preprocess()` consults them to suppress a `P_IllegalChar` whose source
    // byte is in a DEAD branch -- using THIS pass's authoritative liveness so the
    // oracle can never disagree with the real branch decision.
    [[nodiscard]] std::vector<std::pair<ByteOffset, ByteOffset>> const&
    deadRanges() const noexcept {
        return deadRanges_;
    }

    // FC15a (A2): the accumulated `#`/`##` PRODUCT spellings, to be appended to
    // the synth text (AFTER `synth_`'s prefix) before the FINAL buffer is frozen.
    // Empty when no product was generated (then the final buffer == the prefix,
    // byte-identical to the FC14 behavior).
    [[nodiscard]] std::string const& productText() const noexcept {
        return productText_;
    }

    // TF-C82 (D-PP-PRAGMA-REGISTRY): synth byte offset of an emitted token -> the
    // `#pragma pack` member-alignment cap in effect when it was emitted. EMPTY
    // for every TU that uses no `#pragma pack` (i.e. all of them until this
    // cycle), which is what makes the whole mechanism free when unused.
    [[nodiscard]] std::unordered_map<std::uint32_t, std::uint32_t> const&
    pragmaPackByOffset() const noexcept {
        return packByOffset_;
    }

    // TF-C85 (`optimizerControl`): the synth byte offsets of tokens emitted
    // inside a `#pragma optimize("", off)` region. EMPTY for every TU that uses
    // no `#pragma optimize` — the same free-when-unused property as the pack map.
    [[nodiscard]] std::unordered_set<std::uint32_t> const&
    pragmaNoOptimizeByOffset() const noexcept {
        return noOptimizeByOffset_;
    }

    // TF-C82: a `#pragma pack(push, N)` never closed by a `pack(pop)` at end of
    // translation unit. The `#endif`-balance argument applies verbatim: an
    // unbalanced push means the source's own pairing is broken, so `run()` fails
    // loud rather than letting the imbalance read as intentional.
    [[nodiscard]] bool packStackUnbalanced() const noexcept {
        return !packStack_.empty();
    }

    std::vector<Token> run(std::vector<Token> const& in) {
        // c18 (positional macro expansion, C 6.10.3): `body` accumulates the live
        // non-directive tokens since the LAST flush; `out` accumulates the
        // positionally-expanded result. A `#define`/`#undef` only affects text
        // AFTER it, so the pending `body` is FLUSHED through `expand()` (with the
        // table as it stands BEFORE the directive mutates it) at each
        // table-mutating directive boundary -- a use before a later same-name
        // `#define` is then NOT retroactively replaced (the bug SQLite's
        // declare-then-`#define name 0` omit pattern exposed). Pre-c18 this
        // collected the WHOLE body and expanded once at EOF with the FINAL table.
        std::vector<Token> body;
        std::vector<Token> out;
        std::size_t i = 0;
        // c17 (authoritative dead-regions): track the OPEN dead byte-span as the
        // loop crosses conditional directives. A dead span OPENS at the END of a
        // controlling directive line that turns the stack inactive and CLOSES at
        // the START (`#`) of the directive that reactivates it -- one contiguous
        // byte interval covering EVERY byte of the dead branch, INCLUDING
        // dead-branch directive LINES (e.g. a `#define X $` nested in `#if 0`,
        // whose tokens `handleDirective` consumes without pushing). The verdict is
        // the AUTHORITATIVE `stackActive()` (full `table_`+`predefined_`), so the
        // illegal-char oracle agrees with the real branch decision. The
        // controlling directive line stays OUT of the range (it opens at the line
        // END), so a live `#if 1` whose own line carries an illegal char still
        // reports, and a per-directive re-sync (every `#if`/`#elif`/`#else`/
        // `#endif` is a transition check) keeps a live-outer/dead-inner nest from
        // swallowing the live arm.
        bool       inDead    = false;
        ByteOffset deadStart = 0;
        while (i < in.size()) {
            if (isHash(in[i]) && firstOnLine(in, i)) {
                // c18: FLUSH before a directive that mutates the macro table, so
                // the pending body expands against the PRE-directive table. Only
                // `#define`/`#undef` (while the stack is active) mutate it -- an
                // `#include` line is a pass-through (its descriptor macros were
                // spliced earlier in SynthBuilder), conditionals only read the
                // table, and a dead-branch define is not processed. A harmless
                // over-fire is possible (a REJECTED `#define __LINE__ …` keeps
                // `word==define` yet does not mutate the table) -- it only re-
                // segments where expand() is called. That is output-neutral for
                // conforming code (`productText_` is append-only and a hide set
                // never spans a directive line); the only observable effect is on
                // a function-like CALL whose argument list spans the directive
                // line -- undefined behavior (C 6.10.3p11) -- which the extra
                // flush turns from a mis-expansion into a fail-loud unterminated-
                // argument error. Never a silent miscompile.
                if (isFlushBoundaryDirective(in, i)) {
                    flushExpand(body, out);
                    body.clear();
                }
                bool const        wasActive = stackActive();
                ByteOffset const  dirByte   = in[i].span.start();
                std::size_t const next      = handleDirective(in, i, body);
                bool const        nowActive = stackActive();
                if (wasActive && !nowActive) {
                    // active -> dead: the dead branch begins just AFTER this
                    // controlling directive's line.
                    inDead    = true;
                    deadStart = (next > 0 && next <= in.size())
                                    ? in[next - 1].span.end()
                                    : static_cast<ByteOffset>(synth_->size());
                } else if (!wasActive && nowActive && inDead) {
                    // dead -> active: close the span at this reactivating
                    // directive's `#`.
                    if (dirByte > deadStart) {
                        deadRanges_.emplace_back(deadStart, dirByte);
                    }
                    inDead = false;
                }
                i = next;
                continue;
            }
            // FC14: a non-directive token is emitted to the body ONLY when every
            // enclosing conditional branch is active (C 6.10.1 conditional
            // elision). A dead-branch token is dropped here -- so elision
            // precedes `expand` naturally (the dead tokens never reach it).
            if (stackActive()) body.push_back(in[i]);
            ++i;
        }
        // c17: close a dead span still open at EOF (an unterminated `#if 0`). The
        // missing-`#endif` error still fires below; covering the dead bytes up to
        // EOF stops a dead illegal char there from double-reporting on top of it.
        if (inDead) {
            ByteOffset const endB = static_cast<ByteOffset>(synth_->size());
            if (endB > deadStart) deadRanges_.emplace_back(deadStart, endB);
            inDead = false;
        }
        // FC14: an unterminated conditional (a `#if`/`#ifdef`/`#ifndef` with no
        // matching `#endif`) is a constraint violation (C 6.10p1) -- fail loud
        // rather than silently eliding the rest of the file.
        if (!condStack_.empty()) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   SourceSpan::empty(
                       static_cast<ByteOffset>(synth_->size())),
                   "unterminated conditional directive (missing #endif)");
        }
        // TF-C82: the `#pragma pack` analogue of the missing-`#endif` check. A
        // push with no pop means the alignment in effect over the tail of the TU
        // is not derivable from the source's own pairing, so it fails loud for
        // the same reason — and unlike the conditional case it would otherwise be
        // completely invisible.
        if (!packStack_.empty()) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                   SourceSpan::empty(static_cast<ByteOffset>(synth_->size())),
                   std::string{"unterminated '#pragma pack(push, ...)' — "}
                       + std::to_string(packStack_.size())
                       + " push(es) reach end of translation unit with no "
                         "matching pop, so the alignment applied to everything "
                         "below them is unbalanced");
        }
        // c18: final flush of the tokens following the last table-mutating
        // directive (or the whole body, when there was none -- byte-identical to
        // the pre-c18 single end-expand).
        flushExpand(body, out);
        return out;
    }

    // c18 (positional macro expansion): expand `pending` with the CURRENT `table_`
    // and APPEND the non-placemarker results to `accumulated`. Called at each
    // table-mutating directive boundary and once at EOF, so each run expands
    // against the macro state that held AT ITS POSITION (C 6.10.3). A no-op for an
    // empty `pending`. `productText_` is shared across every flush: it is
    // APPEND-ONLY (a product token's span is the absolute `prefixLen_ + offset`,
    // never rewound), and `preprocess()` appends the whole `productText_` exactly
    // once, so a `#`/`##` product minted in one flush keeps a valid span after
    // later flushes (or an `#if`-operand expansion inside `handleDirective`) grow
    // the buffer further.
    void flushExpand(std::vector<Token> const& pending,
                     std::vector<Token>&       accumulated) {
        if (pending.empty()) return;
        // Lift each token via fromToken() so it seeds its OWN synth offset as the
        // invocation anchor (a bare `__LINE__`/`__FILE__` resolves against its real
        // source position; a macro splice inherits the invoking token's anchor) and
        // starts with an EMPTY hide set -- a hide set is per-expansion-run and never
        // needs to span a flush (a directive line terminates the surrounding
        // construct; a function-like call spanning it fails loud -- in collectArgs
        // when the name+`(` are in this flush, else at the parser when only the
        // name precedes the directive -- never a silent mis-expansion).
        // `liftRun` also stamps each token's `spacedBefore` from the trivia this run
        // still carries -- the ONE production point for source-origin adjacency.
        std::vector<ExpToken> work = liftRun(pending);
        // TF-C82: THIS is the expansion whose output reaches the parser, so it is
        // the one whose tokens carry a `#pragma pack` cap. The `#if`-operand
        // expansions (`expandTokens`) run with the flag clear — their tokens are
        // folded into an integer and discarded, and recording them would put
        // offsets in the map that no tree node can ever reference.
        recordPack_ = true;
        std::vector<ExpToken> expanded = expand(std::move(work), 0);
        recordPack_ = false;
        // FC15 paste residuals: a placemarker is normally consumed inside
        // `substitute`; this BACKSTOP drops any stray one so it never reaches the
        // parser as a garbage (default-constructed) token.
        for (ExpToken const& et : expanded) {
            if (et.placemarker) continue;
            accumulated.push_back(et.tok);
        }
    }

    // c18: TRUE iff the directive line at `hashIdx` changes POSITIONAL state that
    // the pending body must not be expanded under — a table-MUTATING
    // `#define`/`#undef`, or (TF-C82) a `#pragma`, while the conditional stack is
    // active (so it would actually be processed -- a dead-branch define is gated
    // out in `handleDirective`). Pure: mirrors `handleDirective`'s own skipTrivia
    // + word-read (so it sees the SAME directive word) without mutating any
    // state. `run()` consults it to flush the pending body BEFORE the state
    // changes.
    //
    // ★ TF-C82 — WHY `#pragma` JOINED THIS SET, AND WHAT BREAKS WITHOUT IT. The
    // pack cap is stamped onto tokens when `flushExpand` emits them, and a flush
    // covers everything accumulated since the previous boundary. Without a flush
    // here, `struct A {...} #pragma pack(4) struct B {...}` would expand A and B
    // in ONE run and stamp BOTH with the post-pragma cap — A would silently be
    // laid out packed. It is the same positional argument `#define` already makes
    // (c18: a use before a later same-name define must not be retroactively
    // replaced), applied to the other piece of state a directive can move.
    // Including EVERY `#pragma`, not just the packing ones, is deliberate: the
    // cheap over-fire is an extra flush (output-neutral — `productText_` is
    // append-only and a hide set never spans a directive line), whereas the cheap
    // under-fire is a wrong struct layout.
    [[nodiscard]] bool isFlushBoundaryDirective(std::vector<Token> const& in,
                                                std::size_t hashIdx) const {
        if (!stackActive()) return false;
        const std::size_t end = lineEnd(in, hashIdx);
        const std::size_t p   = skipTrivia(in, hashIdx + 1);
        if (p >= end || isNewline(in[p])) return false;
        const std::string_view w = text(in[p]);
        return w == cfg().defineDirective || w == cfg().undefDirective
            || (!cfg().pragmaDirective.empty() && w == cfg().pragmaDirective);
    }

private:
    PreprocessConfig const& cfg() const { return schema_->preprocess(); }
    // Slice a token's lexeme. FC15a (A2): a token whose span begins at-or-after
    // the prefix length is a `#`/`##` PRODUCT -- its bytes live in `productText_`
    // (offset by `prefixLen_`), not yet in `synth_`. Every ORIGINAL token spans
    // the prefix (`< prefixLen_`) and slices `synth_` as before. (When no product
    // exists `productText_` is empty and this is byte-identical to the prior
    // single-slice form.)
    std::string_view text(Token const& t) const {
        if (t.span.start() >= prefixLen_) {
            const ByteOffset s = t.span.start() - prefixLen_;
            const ByteOffset e = t.span.end() - prefixLen_;
            if (e <= productText_.size()) {
                return std::string_view{productText_}.substr(s, e - s);
            }
            return {};   // defensive: never UB on a malformed product span
        }
        return synth_->slice(t.span);
    }
    // D-CPP-ERROR-WARNING (C23 6.10.5 / 6.10.6): the VERBATIM `pp-tokens` operand
    // of a diagnostic directive, as source bytes. `p` is the index just past the
    // directive WORD; `end` is `lineEnd`'s one-past-the-newline.
    //
    // NOT macro-expanded, deliberately (gcc and clang agree): 6.10.5 asks only for
    // a message "that includes the specified sequence of preprocessing tokens",
    // and the operand is PROSE. Expanding it would rewrite the author's sentence
    // and could even trip the function-like-macro arity machinery on an ordinary
    // English parenthesis.
    //
    // An EMPTY operand is LEGAL, not malformed: the grammar is
    // `# error pp-tokens_opt`, so a bare `#error` still fires. This returns "" and
    // the caller must NOT bolt on a malformed-operand check.
    //
    // TRAILING-TRIVIA POLICY: the span is clipped to the first and LAST non-trivia
    // token, so trailing whitespace and a trailing comment are EXCLUDED. That is
    // the standard-correct reading rather than a nicety -- a comment becomes one
    // space in translation phase 3, BEFORE phase-4 directive execution, so it is
    // never one of the `pp-tokens` the message must include (`#error nope // TODO`
    // must not report "nope // TODO"). Between those two anchors the slice stays
    // BYTE-VERBATIM, so an INTERIOR comment (`#error a /* x */ b`) does survive:
    // keeping the author's exact spelling and spacing is worth more to the reader
    // than re-spelling the token sequence, and interior comments in a diagnostic
    // message are vanishingly rare.
    //
    // Slices `synth_` DIRECTLY rather than going through `text()`: `text()` takes a
    // `Token const&`, not a span. Its `productText_` branch is UNREACHABLE here
    // anyway -- a `#`/`##` product token can only be born inside a macro
    // replacement list, and a macro expansion can never produce a directive
    // (C 6.10.3p11: a `#` arising from expansion is not a directive), so every
    // token on a directive line is an ORIGINAL one whose span precedes `prefixLen_`.
    [[nodiscard]] std::string_view directiveOperandText(
        std::vector<Token> const& in, std::size_t p, std::size_t end) const {
        std::size_t const first = skipTrivia(in, p);
        if (first >= end || isNewline(in[first])) return {};
        std::size_t last = end;   // one past the newline that ends the line
        while (last > first
               && (isNewline(in[last - 1]) || isTrivia(in[last - 1]))) {
            --last;
        }
        if (last <= first) return {};   // defensive: `first` is already non-trivia
        // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN (CLOSED): the join is CORRECT BY
        // CONSTRUCTION now, and the `"`/`'`/`>` byte re-consume that used to sit
        // here is DELETED. `last - 1` is the last NON-TRIVIA token on the line, and
        // a literal's close delimiter is a significant token of its own, so when
        // the operand ends in a literal the join already ends PAST the delimiter.
        // Previously the closer belonged to no token at all and
        // `#warning "Unsupported compiler detected"` (the `sys/cdefs.h` shape
        // that motivated `#warning`) reported one byte short, violating C23
        // 6.10.5p1/6.10.6's "include the pp-tokens" on the case that matters most;
        // `#error please include <stdio.h>` lost its `>` the same way, because the
        // word `include` arms the tokenizer's header-context. Both are now whole
        // without the engine ever naming a delimiter byte — strictly more agnostic
        // than the guard it replaces, which only knew C's three delimiters.
        SourceSpan const joined =
            SourceSpan::join(in[first].span, in[last - 1].span);
        return synth_->slice(joined);
    }
    bool isHash(Token const& t) const {
        return hashKind_.valid() && t.schemaKind == hashKind_;
    }
    // FC15a: the STRINGIZE (`#`) / TOKEN-PASTE (`##`) operators, by config kind.
    bool isStringize(Token const& t) const {
        return stringizeKind_.valid() && t.schemaKind == stringizeKind_;
    }
    bool isPaste(Token const& t) const {
        return pasteKind_.valid() && t.schemaKind == pasteKind_;
    }
    // A macro NAME / invocation is an identifier-like word. The tokenizer
    // leaves a plain word's schemaKind == InvalidSchemaToken (the PARSER
    // later promotes Word -> Identifier), so the PP keys on the universal
    // `Word` core kind rather than the Identifier schema id. Keywords are
    // also Word-kind but carry a valid schemaKind; a keyword is never in the
    // macro table (a `#define int ...` is the author's error), so matching
    // table membership by text keeps expansion correct.
    static bool isWord(Token const& t) {
        return t.coreKind == CoreTokenKind::Word;
    }

    // ── TF-C82 (D-PP-PRAGMA-REGISTRY): the ONE pragma sink ────────────────────
    //
    // `toks` are a pragma's SIGNIFICANT pp-tokens — for `#pragma X ...` the line
    // after the directive word, for `_Pragma("X ...")` the DE-STRINGIZED operand
    // re-tokenized. BOTH spellings arrive here, which is what makes
    // `_Pragma("pack(4)")` and `#pragma pack(4)` the same feature rather than two
    // implementations that agree until they don't.
    //
    // ★ CALLED ONLY FROM REACHABLE POSITIONS. `handleDirective`'s arm sits BELOW
    // the dead-branch gate and `expand` never runs on elided tokens, so a pragma
    // in a not-taken `#if` branch never reaches this function at all — reachability,
    // not recognition (C 6.10p1), the `#error`/`#embed`/`#line` parity. Hoisting
    // recognition above that gate would fail loud on the hundreds of pragmas the
    // Apple SDK parks in unsupported-configuration branches.
    void applyPragma(std::span<Token const> toks, SourceSpan diagSpan) {
        std::vector<std::string> words;
        std::vector<Token>       sig;
        for (Token const& t : toks) {
            if (isTrivia(t) || isNewline(t)) continue;
            if (t.coreKind == CoreTokenKind::Eof) continue;
            sig.push_back(t);
            words.emplace_back(text(t));
        }
        // C 6.10.6: `# pragma pp-tokens_opt` — a pragma with NO tokens has no
        // effect, so it is silent. This is the standard saying nothing happened,
        // not DSS declining to look.
        if (words.empty()) return;

        auto const m = matchPragmaEffect(cfg(), words);
        if (!m.has_value()) {
            if (!cfg().unknownPragmaIsError) return;   // the declared opt-out
            std::string joined;
            for (auto const& w : words) {
                if (!joined.empty()) joined += ' ';
                joined += w;
            }
            emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                   diagSpan,
                   std::string{"unrecognized pragma '"} + joined
                       + "' — no 'preprocess.pragmaEffects' row claims it, so "
                         "this implementation cannot say whether ignoring it is "
                         "safe. C 6.10.6p2 permits ignoring a pragma, not "
                         "assuming one is inert: `pack` alone silently changes "
                         "struct layout. Add a row declaring what it does (or "
                         "set 'unknownPragmaIsError' false to ignore every "
                         "unregistered pragma)");
            return;
        }
        switch (m->effect) {
            case PragmaEffect::DiagnosticsOnly:
            case PragmaEffect::AnnotationOnly:
            case PragmaEffect::RealizationRequestOnly:
                // IGNORED — and ignored because a ROW SAYS SO. That is the whole
                // difference from the pre-TF-C82 behavior, which looked identical
                // from the outside and asserted nothing.
                //
                // TF-C85: `RealizationRequestOnly` shares this arm and NOT its
                // meaning. The other two say the pragma does not concern
                // translation; this one says it concerns HOW a name is realized
                // and never WHETHER it exists — so ignoring it cannot mask a
                // missing symbol, because the reference itself is what fails
                // loud, at the call site, in the semantic tier. See
                // `PragmaEffect::RealizationRequestOnly`.
                return;
            case PragmaEffect::StructPacking:
                applyPackPragma(std::span<Token const>{sig}.subspan(m->words),
                                diagSpan);
                return;
            case PragmaEffect::OptimizerControl:
                applyOptimizePragma(
                    std::span<Token const>{sig}.subspan(m->words), diagSpan);
                return;
            case PragmaEffect::Unsupported: {
                std::string joined;
                for (auto const& w : words) {
                    if (!joined.empty()) joined += ' ';
                    joined += w;
                }
                emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                       diagSpan,
                       std::string{"pragma '"} + joined
                           + "' has translation semantics this implementation "
                             "has not built, and its 'preprocess.pragmaEffects' "
                             "row says so ('unsupported'). Ignoring it would "
                             "change behavior silently");
                return;
            }
            case PragmaEffect::StandardFloatState:
                // C23 6.10.8 `standard-pragma`, in a state THIS implementation
                // satisfies — accepted and inert. See the long per-form
                // measurement on `PragmaEffect::StandardFloatState`. Silence
                // here is a CLAIM (a row asserts DSS's behaviour already meets
                // the request), not an omission.
                return;
            case PragmaEffect::StandardFloatStateDiverges: {
                // ★★★ ACCEPTED WITH NOTICE — the TU COMPILES and the unhonoured
                // request is NAMED. Refusing would be below the reference union
                // (all four accept all nine forms, ✔measured); accepting in
                // silence would ship wrong numerics without a word. The notice
                // rides an UNSUPPRESSABLE code at WARNING severity, so it can be
                // neither capped nor `--suppress`ed away, and it does not fail
                // the build.
                std::string joined;
                for (auto const& w : words) {
                    if (!joined.empty()) joined += ' ';
                    joined += w;
                }
                emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                       diagSpan,
                       std::string{"pragma '"} + joined
                           + "' is RECOGNIZED and ACCEPTED, but this "
                             "implementation does not provide the state it asks "
                             "for, and the difference changes floating-point "
                             "RESULTS rather than only performance. Its "
                             "'preprocess.pragmaEffects' row says so "
                             "('standardFloatStateDiverges'). Translation "
                             "continues — every reference compiler accepts this "
                             "pragma, so refusing it would reject a valid "
                             "program — but the request is NOT honoured: DSS "
                             "contracts nothing, takes no floating-point "
                             "environment into account, and evaluates complex "
                             "multiply and divide with the usual algebraic "
                             "formulas (no infinity/NaN recovery)",
                       DiagnosticSeverity::Warning);
                return;
            }
            case PragmaEffect::IncludeOnce:
                // ★★★ D-PP-PRAGMA-RECOGNIZED-SEMANTICS — THIS ARM WAS THE DEFECT,
                // AND IT IS NOW INERT BY CONSTRUCTION RATHER THAN BY OMISSION.
                //
                // Until 2026-08-29 this arm REFUSED the translation unit, which
                // meant every real-world header using the commonest guard idiom
                // failed to compile under DSS against an idiom gcc, clang and
                // MSVC all accept. TF-C87's justification for the refusal —
                // "DSS implements no include-once dedup" — was TRUE when written
                // and is FALSE now: `SynthBuilder` maintains a TU-wide
                // `includeOnceSet` keyed on `core::PathIdentity`, records this
                // file when the pragma is REACHED, and skips a repeat splice.
                //
                // ★★ SILENCE HERE IS NOT A DROPPED PRAGMA. The effect has ALREADY
                // HAPPENED by the time this pass runs — the pre-scan honoured it
                // while splicing, which is the only phase that can. Re-reporting
                // it here would be reporting a fact, not a problem, and refusing
                // here would refuse a program DSS has already handled correctly.
                //
                // ⚠ IF THE PRE-SCAN DID NOT RECORD IT (a conditional this weaker
                // evaluator could not confidently call live), the header is
                // spliced TWICE and the duplicate definitions fail LOUD in the
                // semantic tier. That is the safe direction and it is the one the
                // gate is chosen to fall toward — never a silent text drop.
                return;
        }
    }

    // C `#pragma pack` — the `structPacking` verb's operand grammar. `args` are
    // the significant tokens AFTER the matched registry prefix.
    //
    // The BUILT forms are exactly the ones MEASURED reachable in the sqlite
    // corpus (13 `pack(4)` + 1 `pack(1)`, 14 `pack()`, 5 `pack(push, 4)` + 1
    // `pack(push, 1)`, 6 `pack(pop)`):
    //     pack ( )            -> drop the cap back to the target default
    //     pack ( N )          -> cap member alignment at N
    //     pack ( push , N )   -> save the current cap, then cap at N
    //     pack ( pop )        -> restore the saved cap
    // Everything else FAILS LOUD, deliberately: the SDK's other spellings are the
    // bare `pack(push)` (MEASURED 0 occurrences SDK-wide) and the paren-less
    // `#pragma pack 8` (MEASURED 2, both in an unreached `ffi/ffi.h`). Guessing at
    // an unbuilt form is how a wrong layout ships quietly; refusing it is a
    // one-line config/compiler fix when something finally needs it.
    //
    // AGNOSTIC: the delimiters are matched by SCHEMA KIND (`parenOpen_`,
    // `parenClose_`, `argSep_` — the same config lexemes the function-like macro
    // machinery uses), and `push`/`pop` come from `pragmaPackPushWord` /
    // `pragmaPackPopWord`. No `(`, `)`, `,`, `"push"` or `"pop"` byte appears in
    // this function.
    void applyPackPragma(std::span<Token const> args, SourceSpan diagSpan) {
        auto const bad = [&](std::string_view why) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                   diagSpan,
                   std::string{"unsupported '#pragma pack' form: "}
                       + std::string{why}
                       + ". The built forms are pack(N), pack(), pack(push, N) "
                         "and pack(pop); refusing rather than guessing, because a "
                         "misread pack silently relayouts every struct after it");
        };
        auto const isKind = [&](Token const& t, SchemaTokenId k) {
            return k.valid() && t.schemaKind == k;
        };
        if (args.size() < 2 || !isKind(args.front(), parenOpen_)
            || !isKind(args.back(), parenClose_)) {
            bad("the operand must be a parenthesized list");
            return;
        }
        std::span<Token const> inner = args.subspan(1, args.size() - 2);

        // pack() — reset to the target's natural alignment rules.
        if (inner.empty()) {
            packCurrent_ = 0;
            return;
        }
        // pack(N)
        if (inner.size() == 1 && inner[0].coreKind == CoreTokenKind::IntLiteral) {
            if (auto n = packOperandValue(inner[0], diagSpan)) packCurrent_ = *n;
            return;
        }
        // pack(pop)
        if (inner.size() == 1 && !cfg().pragmaPackPopWord.empty()
            && text(inner[0]) == cfg().pragmaPackPopWord) {
            if (packStack_.empty()) {
                // gcc warns and leaves the cap alone; DSS refuses. An unbalanced
                // pop means the pushes and pops in this TU do not pair, so EVERY
                // composite after it is laid out under a cap nobody can name from
                // reading the source — the definition of a silent wrong layout.
                emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                       diagSpan,
                       "'#pragma pack(pop)' with an empty pack stack — the "
                       "push/pop pairs in this translation unit are unbalanced, "
                       "so the alignment in effect below this line is not "
                       "derivable from the source");
                return;
            }
            packCurrent_ = packStack_.back();
            packStack_.pop_back();
            return;
        }
        // pack(push, N)
        if (inner.size() == 3 && !cfg().pragmaPackPushWord.empty()
            && text(inner[0]) == cfg().pragmaPackPushWord
            && isKind(inner[1], argSep_)
            && inner[2].coreKind == CoreTokenKind::IntLiteral) {
            if (auto n = packOperandValue(inner[2], diagSpan)) {
                packStack_.push_back(packCurrent_);
                packCurrent_ = *n;
            }
            return;
        }
        // A `push`/`pop` spelling the language did NOT declare lands here — the
        // red-on-disable pin for `pragmaPackPushWord`/`pragmaPackPopWord`.
        bad("operand is not one of the built forms");
    }

    // Decode + VALIDATE a `#pragma pack` alignment operand. Must be a positive
    // power of two no greater than 256 — the same envelope `alignas` /
    // `__attribute__((aligned(N)))` are held to, because it lands in the same
    // layout channel and a value the layout engine cannot represent would abort
    // deep in `computeLayout` with no source position.
    [[nodiscard]] std::optional<std::uint32_t> packOperandValue(
        Token const& t, SourceSpan diagSpan) {
        std::string_view const lex = text(t);
        std::uint64_t          v   = 0;
        for (char const c : lex) {
            if (c < '0' || c > '9') { v = 0; break; }
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
            if (v > 4096) break;   // far past the cap; stop before overflow
        }
        if (v == 0 || v > 256 || (v & (v - 1)) != 0) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                   diagSpan,
                   std::string{"'#pragma pack' alignment '"} + std::string{lex}
                       + "' must be a power of two in [1, 256]");
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(v);
    }

    // ★★ TF-C85: MSVC `#pragma optimize` — the `optimizerControl` verb's operand
    // grammar. `args` are the significant tokens AFTER the matched registry
    // prefix.
    //
    // The BUILT forms are exactly the ones MEASURED reachable in the sqlite
    // corpus (the two `#pragma optimize` directives in `ext/misc/totype.c`):
    //     optimize ( "" , off )   -> open a no-optimize region
    //     optimize ( "" , on )    -> close it
    // The EMPTY option string is MSVC's "all optimizations". A NON-EMPTY one
    // (`"gt"`, `"s"`, …) names a SELECTIVE subset of MSVC's own optimization
    // vocabulary, which has no DSS translation at all — so it FAILS LOUD rather
    // than being silently widened to "all off" (too pessimistic) or "ignored"
    // (silently keeps a pass the source disabled). Same posture as the unbuilt
    // `#pragma pack` spellings.
    //
    // ★ WHAT THIS SINK HONESTLY IS — DO NOT OVERSTATE IT. It is FAITHFULNESS to
    // a real MSVC contract, and it is what unblocks the pe64 corpus leg. It is
    // NOT a fix for a live floating-point miscompile. MEASURED in this tree: no
    // pass in the shipped release pipeline can perturb float arithmetic —
    // `const_fold.cpp`'s fold maps are integer-only (no FAdd/FMul/FDiv/FCmp/
    // SIToFP arms), there is no reassociation pass, there is no FMA/fast-math
    // anywhere, and `double` is SSE2 at exactly 64 bits (x87 is F80-only). So
    // totype.c's ACTUAL hazard — x87 excess precision defeating its
    // round-trip-equality test — structurally cannot occur here. The sink exists
    // because the source says "do not optimize this function" and DSS now
    // records and honors that, not because ignoring it was miscompiling today.
    //
    // ★ NO END-OF-TU BALANCE CHECK, DELIBERATELY — AND THIS IS WHERE IT DIVERGES
    // FROM `pack`. `pack(push)` opens a STACK that must pair, so an unmatched
    // push leaves an alignment nobody can name from the source and fails loud.
    // `optimize("", off)` is a flat SWITCH: MSVC defines a region with no closing
    // `on` as running to end of file, so refusing it would reject a legal
    // program. Different construct, different rule.
    //
    // AGNOSTIC: the delimiters are matched by SCHEMA KIND (`parenOpen_`,
    // `parenClose_`, `argSep_`, and `quoteIncludeKind_` for the string-literal
    // opener — the same kinds the macro machinery and `_Pragma` already use), and
    // `on`/`off` come from `pragmaOptimizeOnWord` / `pragmaOptimizeOffWord`. No
    // `(`, `)`, `,`, `"`, `"on"` or `"off"` byte appears in this function.
    void applyOptimizePragma(std::span<Token const> args, SourceSpan diagSpan) {
        auto const bad = [&](std::string_view why) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                   diagSpan,
                   std::string{"unsupported '#pragma optimize' form: "}
                       + std::string{why}
                       + ". The built forms are optimize(\"\", off) and "
                         "optimize(\"\", on); refusing rather than guessing, "
                         "because a misread optimize pragma either re-enables "
                         "passes the source disabled or disables passes it "
                         "never mentioned");
        };
        auto const isKind = [&](Token const& t, SchemaTokenId k) {
            return k.valid() && t.schemaKind == k;
        };
        if (args.size() < 2 || !isKind(args.front(), parenOpen_)
            || !isKind(args.back(), parenClose_)) {
            bad("the operand must be a parenthesized list");
            return;
        }
        std::span<Token const> inner = args.subspan(1, args.size() - 2);
        // `"" , <word>` — at minimum a literal opener, its (empty) body, the
        // separator and the state word.
        if (inner.size() < 4 || !isKind(inner[inner.size() - 2], argSep_)) {
            bad("the operand must be an option string, a separator and a state "
                "word");
            return;
        }
        // The option string: everything before the separator. A string literal
        // lexes as OPENER + BODY (+ CLOSER), matched by the CONFIG kind, exactly
        // as `consumePragmaOperator` matches a `_Pragma` operand — never by the
        // `"` byte.
        std::span<Token const> opt = inner.subspan(0, inner.size() - 2);
        if (opt.size() < 2 || opt.size() > 3
            || !quoteIncludeKind_.valid()
            || opt[0].schemaKind != quoteIncludeKind_) {
            bad("the first operand must be a string literal");
            return;
        }
        if (!text(opt[1]).empty()) {
            bad("only the EMPTY option string (MSVC's \"all optimizations\") is "
                "built — a selective option list names optimizations this "
                "implementation does not have");
            return;
        }
        // The state word. Both spellings come from config and are INDEPENDENT:
        // a language that declares only one gets the other refused as unbuilt,
        // which is this pair's red-on-disable pin.
        std::string_view const word = text(inner.back());
        if (!cfg().pragmaOptimizeOffWord.empty()
            && word == cfg().pragmaOptimizeOffWord) {
            optimizeOff_ = true;
            return;
        }
        if (!cfg().pragmaOptimizeOnWord.empty()
            && word == cfg().pragmaOptimizeOnWord) {
            optimizeOff_ = false;
            return;
        }
        bad("the state word is not one the language declares "
            "('pragmaOptimizeOnWord' / 'pragmaOptimizeOffWord')");
    }

    // TF-C85: stamp the `#pragma optimize` state onto an EMITTED token, at the
    // SAME chokepoint and for the same reason as `notePackForToken` — an entity
    // minted by a macro inside a region otherwise carries the `#define` line's
    // span, which is nowhere near the region. Sparse: outside a region nothing is
    // recorded, so the set stays empty and the mechanism is free when unused.
    void noteNoOptimizeForToken(Token const& t) {
        if (!optimizeOff_) return;
        noOptimizeByOffset_.insert(static_cast<std::uint32_t>(t.span.start()));
    }

    // ── TF-C82 (`_Pragma`; C 6.10.9): the OPERATOR spelling ───────────────────
    //
    // `work.front()` is a token whose lexeme matched `pragmaOperator`. Consume
    // `( "..." )`, DE-STRINGIZE the operand per 6.10.9p1, re-tokenize it through
    // the SAME `materializeSignificant` the `#`/`##` products use, and route the
    // result through the SAME `applyPragma`. Returns TRUE when the whole
    // construct was consumed (the caller must `continue`); FALSE leaves `work`
    // untouched so the token falls through to ordinary macro handling.
    //
    // ★★ IT LIVES HERE, IN `expand`, AND THAT IS THE WHOLE POINT. `_Pragma` is an
    // OPERATOR in the token stream, not a directive, so it routinely arrives from
    // inside a macro REPLACEMENT LIST — `sys/queue.h`'s
    // `__NULLABILITY_COMPLETENESS_PUSH/POP` are exactly that, expanded at 40 use
    // sites, and MEASURED they are what puts 24 `clang assume_nonnull` +
    // 2 `clang diagnostic ignored "-Wnullability-completeness"` lines in the
    // corpus's REACHED pragma set. Routing `_Pragma` at the directive scan
    // instead would leave a file-scope `_Pragma` working while every macro-borne
    // one silently vanished — a halfway state that looks green from the outside,
    // which is why the test battery discriminates on exactly that shape.
    //
    // NOTHING is emitted: like a directive, a pragma is not program text.
    [[nodiscard]] bool consumePragmaOperator(std::deque<ExpToken>& work,
                                             ExpToken const&       opTok) {
        std::size_t const openIdx = nextSignificant(work, 1);
        if (openIdx >= work.size() || !isParenOpen(work[openIdx].tok)) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                   opTok.tok.span,
                   std::string{"'"} + cfg().pragmaOperator
                       + "' must be followed by a parenthesized string literal "
                         "(C 6.10.9p1)");
            return false;   // not a call shape — leave it to ordinary handling
        }
        // Find the matching close paren (balanced, so a `)` inside the literal's
        // own body cannot terminate the operand early).
        std::size_t closeIdx = 0;
        int         depthP   = 0;
        bool        found    = false;
        for (std::size_t k = openIdx; k < work.size(); ++k) {
            if (isParenOpen(work[k].tok)) ++depthP;
            else if (isParenClose(work[k].tok)) {
                if (--depthP == 0) { closeIdx = k; found = true; break; }
            }
        }
        if (!found) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                   opTok.tok.span,
                   std::string{"unterminated '"} + cfg().pragmaOperator
                       + "' operand (no closing parenthesis)");
            return false;
        }
        // The operand: the significant tokens strictly between the parens. A
        // string literal lexes as OPENER + BODY (+ CLOSER since
        // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN), so the shape is 2 or 3 tokens.
        // The opener is matched by the CONFIG kind — the same `quoteIncludeKind_`
        // `handleEmbed` reuses as "this language's string-literal opener" — never
        // by the `"` byte.
        std::vector<Token> operand;
        for (std::size_t k = openIdx + 1; k < closeIdx; ++k) {
            if (isTrivia(work[k].tok) || isNewline(work[k].tok)) continue;
            operand.push_back(work[k].tok);
        }
        bool const wellFormed = operand.size() >= 2 && operand.size() <= 3
                                && quoteIncludeKind_.valid()
                                && operand[0].schemaKind == quoteIncludeKind_;
        if (!wellFormed) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPragma, synth_->id(),
                   opTok.tok.span,
                   std::string{"the operand of '"} + cfg().pragmaOperator
                       + "' must be a single string literal (C 6.10.9p1) — "
                         "refusing rather than guessing at its intent");
            // Still CONSUME the construct: leaving `_Pragma ( ... )` in the token
            // stream would cascade into an inscrutable parse error on top of this
            // accurate one.
            for (std::size_t k = 0; k <= closeIdx; ++k) work.pop_front();
            return true;
        }
        // 6.10.9p1: `\"` -> `"` and `\\` -> `\`. The BODY token's text already
        // excludes the delimiters (an `L` prefix rides on the opener), so this is
        // exactly the escape pass and nothing more — a full string decoder would
        // silently turn a `\n` a pragma legitimately contains into a newline.
        std::string const inner = destringizePragma(text(operand[1]));
        // [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: the de-stringized operand is
        // minted like any other product. Nothing is EMITTED from it (a pragma is
        // not program text), so no parse diagnostic can land on these bytes today
        // — the stamp is here because the chokepoint's contract is that every
        // minted run has provenance, not because this run is known to need it.
        MintScope const mintScope{*this, opTok.invOffset, 0, /*hasDef=*/false,
                                  cfg().pragmaOperator};
        std::vector<Token> const toks = materializeSignificant(inner);
        applyPragma(toks, opTok.tok.span);
        for (std::size_t k = 0; k <= closeIdx; ++k) work.pop_front();
        return true;
    }

    // TF-C82: stamp the pack cap in effect onto an EMITTED token. Sparse — a zero
    // cap (every token in every TU that uses no `#pragma pack`) records nothing,
    // so the map stays empty and the whole mechanism is free when unused.
    //
    // ★★ THE CONFLICT ARM RECORDS, IT DOES NOT REPORT — AND THAT DISTINCTION WAS
    // MEASURED, NOT REASONED. One source token can reach the output twice under
    // DIFFERENT caps: a macro replacement expanded inside two different pack
    // regions. The first implementation failed loud right here, and it REFUSED A
    // PROGRAM CLANG COMPILES — a shared MEMBER macro (`#define MEMS unsigned a;
    // long long b;`) used in two pack regions stamps its own tokens under both
    // caps, while every composite that uses it is anchored on an UNAMBIGUOUS
    // `struct` keyword and lays out perfectly. The preprocessor cannot tell which
    // offsets are used as a layout key; only the semantic tier can. So the
    // ambiguity is RECORDED as `kPackAmbiguous` and judged there — it becomes an
    // error (`S_PragmaPackAmbiguous`) only if a composite actually lands on it.
    // Sticky: once ambiguous, a third stamp cannot resolve it back.
    void notePackForToken(Token const& t) {
        if (packCurrent_ == 0) return;
        auto const key = static_cast<std::uint32_t>(t.span.start());
        auto const [it, fresh] = packByOffset_.try_emplace(key, packCurrent_);
        if (fresh || it->second == packCurrent_) return;
        it->second = kPragmaPackAmbiguous;
    }

    bool firstOnLine(std::vector<Token> const& in, std::size_t idx) const {
        for (std::size_t p = idx; p-- > 0;) {
            if (isNewline(in[p])) return true;
            if (!isTrivia(in[p])) return false;
        }
        return true;
    }

    static std::size_t skipTrivia(std::vector<Token> const& in, std::size_t p) {
        while (p < in.size() && isTrivia(in[p]) && !isNewline(in[p])) ++p;
        return p;
    }
    static std::size_t lineEnd(std::vector<Token> const& in, std::size_t p) {
        while (p < in.size() && !isNewline(in[p])) ++p;
        if (p < in.size()) ++p;
        return p;
    }
    // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN: `bodyIdx` indexes a coalesced literal
    // BODY token; return the index just past it AND past the CLOSE-delimiter token
    // that now follows. Every directive handler that reads a quoted operand used
    // to step `body + 1` straight onto whatever came next, because the closing
    // delimiter belonged to no token; that step now lands ON the closer, which is
    // a SIGNIFICANT token (deliberately un-flagged so a shape can name the slot)
    // and therefore reads as trailing junk to the strict "nothing may follow"
    // checks. The closer KIND is resolved from the schema through the body token's
    // own kind — the engine never spells `"StringEnd"`. Tolerant of a language
    // that declares no closer: the index then just steps past the body.
    [[nodiscard]] std::size_t pastBodyAndCloser(std::vector<Token> const& in,
                                                std::size_t bodyIdx) const {
        const SchemaTokenId closeKind =
            closeTokenForCoalescedBody(*schema_, in[bodyIdx].schemaKind);
        std::size_t n = bodyIdx + 1;
        if (closeKind.valid() && n < in.size() && in[n].schemaKind == closeKind) {
            ++n;
        }
        return n;
    }
    std::size_t handleDirective(std::vector<Token> const& in, std::size_t start,
                                std::vector<Token>& body) {
        const std::size_t end = lineEnd(in, start);
        std::size_t p = skipTrivia(in, start + 1);
        if (p >= end || isNewline(in[p])) return end;
        const std::string_view word = text(in[p]);

        // FC14 (MF-3): the conditional-compilation directives are dispatched
        // UNCONDITIONALLY -- they must always update `condStack_` so nesting is
        // tracked correctly even inside a dead branch (an `#if` nested in an
        // elided `#if 0` still needs its matching `#endif` to balance). Their
        // operand is evaluated ONLY when the branch should be (handled inside).
        // ★ This UNCONDITIONAL dispatch (BEFORE the `stackActive()` gate below)
        // is what closes the C23 `#elifdef`/`#elifndef` silent miscompile: an
        // unrecognized `#elifdef` would otherwise never update the frame, its
        // true branch would be skipped, and control would silently fall to
        // `#else` (D-PP-ELIFDEF-ELIFNDEF).
        if (word == cfg().ifDirective) {
            handleIf(in, p + 1, end, /*kind=*/IfKind::Expr);
            return end;
        }
        if (word == cfg().ifdefDirective) {
            handleIf(in, p + 1, end, IfKind::Ifdef);
            return end;
        }
        if (word == cfg().ifndefDirective) {
            handleIf(in, p + 1, end, IfKind::Ifndef);
            return end;
        }
        if (word == cfg().elifDirective) {
            handleElif(in, p + 1, end, IfKind::Expr);
            return end;
        }
        // C23 (D-PP-ELIFDEF-ELIFNDEF; C 6.10.1p5): `#elifdef X` == `#elif
        // defined(X)`, `#elifndef X` == `#elif !defined(X)` -- routed to the same
        // `handleElif` with the DIRECT definedness kind. OPTIONAL words, guarded
        // by `.empty()` so a stripped/pre-C23 config leaves the directive to the
        // unsupported-directive fail-loud below (never a silent branch skip).
        if (!cfg().elifdefDirective.empty() && word == cfg().elifdefDirective) {
            handleElif(in, p + 1, end, IfKind::Ifdef);
            return end;
        }
        if (!cfg().elifndefDirective.empty()
            && word == cfg().elifndefDirective) {
            handleElif(in, p + 1, end, IfKind::Ifndef);
            return end;
        }
        if (word == cfg().elseDirective) {
            handleElse(in[p].span);
            return end;
        }
        if (word == cfg().endifDirective) {
            handleEndif(in[p].span);
            return end;
        }

        // FC14 (MF-3): every NON-conditional directive -- AND its diagnostics --
        // is GATED on the conditional stack being active. Inside a dead branch
        // an unknown/malformed directive is NOT an error (C 6.10p1: a skipped
        // group's directives are only parsed enough to track nesting), so the
        // whole arm (including the unsupported-directive diagnostic) is skipped.
        if (!stackActive()) return end;

        // FC15c / ★★ TF-C82 (D-PP-PRAGMA-REGISTRY; `#pragma`, C 6.10.6): the
        // line's tokens are never emitted into `body` (a directive is not program
        // text), but WHAT THE PRAGMA MEANS is now decided by the
        // `preprocess.pragmaEffects` registry instead of being dropped in
        // silence. Placed AFTER the dead-branch gate (so a `#pragma` in an elided
        // branch is entirely silent — C 6.10p1, the `#error`/`#embed`/`#line`
        // parity) and BEFORE the generic unsupported-directive `else`.
        // Config-matched (`pragmaDirective`), never a hard-coded "pragma"; an
        // empty config field (a language without `#pragma`) skips this arm -> the
        // line falls through to the generic unsupported-directive fail-loud.
        //
        // ★ WHAT THIS ARM USED TO BE: `return end;`. MEASURED, that silence cost
        // `sys/fcntl.h`'s `struct log2phys` its `#pragma pack(4)` — 24 bytes where
        // clang says 20, on a struct sqlite hands to `fcntl(F_LOG2PHYS)`.
        if (!cfg().pragmaDirective.empty() && word == cfg().pragmaDirective) {
            std::size_t const first = skipTrivia(in, p + 1);
            std::size_t       last  = end;
            while (last > first && (isNewline(in[last - 1])
                                    || isTrivia(in[last - 1]))) --last;
            if (last > first) {
                applyPragma(std::span<Token const>{in}.subspan(first, last - first),
                            in[p].span);
            }
            return end;
        }

        if (word == cfg().defineDirective) {
            handleDefine(in, p + 1, end);
        } else if (word == cfg().undefDirective) {
            handleUndef(in, p + 1, end);
        } else if (word == cfg().includeDirective) {
            // D-PP-CONDITIONAL-INCLUDE-ORDERING (CLOSED, c17): this arm runs
            // only when the conditional stack is ACTIVE (the dead-branch gate
            // above already returned for an elided include), so it is reached
            // ONLY for a LIVE include -- which `SynthBuilder` resolved/spliced
            // for a quote form and passed through for an angle form (to the
            // post-parse import resolver). The angle line's text is forwarded so
            // the resolver still sees it (its tokens are inert to the parser).
            //
            // TF-C60 (D-PP-PRESCAN-CROSS-BUFFER-MACRO-STATE, the fail-loud
            // half): a QUOTE include reaching THIS arm was NOT spliced by the
            // pre-scan — and this arm is authoritatively LIVE — so forwarding it
            // as inert tokens SILENTLY DROPS the header (the old comment's
            // "fails loud downstream" claim was false; nothing downstream ever
            // failed). Sharing the macro state makes the pre-scan lockstep on
            // ORDINARY object-like guards, so what remains is a guard this
            // weaker evaluator cannot decide. ★ Do NOT name a single cause in
            // the message: the code-audit reached this arm from FIVE distinct
            // ones (a function-like guard, an object-like macro expanding TO a
            // function-like name, a descriptor-injected macro the pre-scan never
            // tracks [D-PP-PRESCAN-DESCRIPTOR-MACROS-UNTRACKED], an expansion
            // chain past the depth backstop, and — before it was fixed — a
            // literal whose closer the replacement text had lost). Naming one
            // sends users chasing the wrong thing.
            {
                std::size_t q = skipTrivia(in, p + 1);
                if (q < end && quoteIncludeKind_.valid()
                    && in[q].schemaKind == quoteIncludeKind_) {
                    std::string_view nameTx =
                        (q + 1 < end && !isNewline(in[q + 1])) ? text(in[q + 1])
                                                               : std::string_view{};
                    emitPP(rep_, DiagnosticCode::P_PreprocessorIncludeError,
                           synth_->id(), in[q].span,
                           std::string{"quote #include \""} + std::string{nameTx}
                               + "\" is LIVE here but the include pre-scan could "
                                 "not evaluate its conditional guard, so the "
                                 "header was never spliced; refusing to silently "
                                 "drop it");
                    return end;
                }
            }
            for (std::size_t q = start; q < end; ++q) body.push_back(in[q]);
        } else if (!cfg().embedDirective.empty()
                   && word == cfg().embedDirective) {
            // FC17.9(h) C23 6.10.4 / N3096 6.10.3 (D-PP-EMBED). Reached ONLY for
            // a LIVE `#embed` (the dead-branch gate above already returned, so a
            // dead-branch `#embed` -- even of a missing file -- is skipped with no
            // resolution/diagnostic, the #define/#include/pragma parity). `p` is
            // the `embed` directive WORD index; handleEmbed derives the resolution
            // dir + all diagnostic positions from it and splices the resource
            // bytes into `body`. Config-matched (`embedDirective`), never a
            // hard-coded "embed"; an empty field (a language with no `#embed`)
            // skips this arm -> the generic unsupported-directive fail-loud below.
            handleEmbed(in, p, end, body);
        } else if (!cfg().lineDirective.empty()
                   && word == cfg().lineDirective) {
            // TF-C59 C23 6.10.4 (D-CPP-LINE-DIRECTIVE). Reached ONLY for a LIVE
            // `#line` (the dead-branch gate above already returned, so a `#line`
            // in an elided branch is skipped with no diagnostic — the
            // #define/#include/#pragma/#embed parity). The line's tokens are NOT
            // forwarded into `body`: a directive is not program text.
            handleLine(in, p + 1, end);
        } else if (!cfg().errorDirective.empty()
                   && word == cfg().errorDirective) {
            // D-CPP-ERROR-WARNING, C23 6.10.5 (`#error`). 6.10.5p1 is a
            // CONSTRAINT: the implementation shall produce a diagnostic message
            // that includes the specified `pp-tokens`. Error severity; the code is
            // in the unsuppressable closed table (an authored abort must not be
            // silenceable into building the very configuration the author declared
            // invalid).
            //
            // ★ ANTI-INSTRUCTION — DO NOT move this arm (or its `#warning` twin)
            // up into the UNCONDITIONAL conditional-dispatch block above (the
            // `#if`/`#ifdef`/.../`#endif` chain). That block deliberately runs in
            // DEAD branches to keep `condStack_` balanced; an `#error` handled
            // there would fire on every LEXED `#error`, including the ones the
            // Apple SDK headers put inside unsupported-configuration branches that
            // a supported target skips — i.e. every macOS compile would break.
            // Sitting HERE, below `if (!stackActive()) return end;`, makes a
            // dead-branch `#error` structurally incapable of firing: reachability,
            // not recognition (C 6.10p1; the #define/#include/#pragma/#embed/#line
            // parity).
            //
            // The operand is VERBATIM + optional (see `directiveOperandText`) and a
            // bare `#error` is well-formed, so there is deliberately NO
            // malformed-operand check. The `"#error: "` prefix is a fixed
            // presentation LABEL naming the C directive class — the MATCH above is
            // on `cfg().errorDirective`, never on a hard-coded spelling, so a
            // config that rebinds the word still routes here (and a config that
            // drops it falls through to the P0015 fail-loud below). The line's
            // tokens are NOT forwarded into `body`: a directive is not program text.
            emitPP(rep_, DiagnosticCode::P_PreprocessorErrorDirective,
                   synth_->id(), in[p].span,
                   std::string{"#error: "}
                       + std::string{directiveOperandText(in, p + 1, end)});
        } else if (!cfg().warningDirective.empty()
                   && word == cfg().warningDirective) {
            // D-CPP-ERROR-WARNING, C23 6.10.6 (`#warning`) — the `#error` twin at
            // Warning severity: 6.10.6 standardises the long-standing gcc/clang
            // extension, and translation CONTINUES, so this must never bump
            // `errorCount()` (hence the explicit severity argument; every other
            // `emitPP` here defaults to Error). Suppressible by design, unlike its
            // `#error` sibling. Same reachability gate, same verbatim-optional
            // operand, same no-tokens-into-`body` rule as above.
            emitPP(rep_, DiagnosticCode::P_PreprocessorWarningDirective,
                   synth_->id(), in[p].span,
                   std::string{"#warning: "}
                       + std::string{directiveOperandText(in, p + 1, end)},
                   DiagnosticSeverity::Warning);
        } else if (cfg().lineMarker.has_value() && isDecimalRun(word)) {
            // D-C-PREPROCESSED-INPUT-REFUSES-GCC-LINEMARKERS. The GNU LINEMARKER
            // `# N "file" [flags]`. Reached ONLY for a LIVE one (the dead-branch
            // gate above already returned — the #define/#include/pragma/#embed/
            // #line parity), and ONLY when the language DECLARES the surface: a
            // config without `lineMarker` leaves a digit-led directive on the
            // unsupported-directive fail-loud below, unchanged.
            //
            // ★ THE PREDICATE IS THE SHAPE, NOT A WORD, AND THAT IS FORCED: the
            // GNU form has no directive word, so there is no lexeme to match a
            // configured spelling against. `isDecimalRun` is therefore the
            // recognizer, and it is placed LAST in this chain so it can never
            // shadow a language whose directive word is spelled with digits.
            // The line's tokens are NOT forwarded into `body`: a directive is not
            // program text.
            handleLineMarker(in, p, end);
        } else {
            emitPP(rep_, DiagnosticCode::P_PreprocessorUnsupported,
                   synth_->id(), in[p].span,
                   std::string{"unsupported preprocessor directive (out of "
                               "FC13 cycle-1 scope): "}
                       + std::string{word});
        }
        return end;
    }

    // FC14 / c17: which `#if`-family directive opened a frame. Aliases the
    // anon-namespace `SbIfKind` so the existing `IfKind::Expr` call sites compile
    // while the open/close logic is the SHARED free `sbHandle*` (Phase 7).
    using IfKind = SbIfKind;

    // True iff every open conditional frame's current branch is active (empty
    // stack => active). The gate for token emission + non-conditional
    // directives. Delegates to the shared `sbStackActive`.
    [[nodiscard]] bool stackActive() const { return sbStackActive(condStack_); }

    // True iff `name` is currently a defined macro (C's `defined X` / `#ifdef`).
    [[nodiscard]] bool isDefined(std::string_view name) const {
        // `#ifdef X` / `#if defined(X)` is TRUE for a `#define`d macro (table_)
        // OR a config-seeded predefined macro (predefined_ — e.g. `_WIN32`,
        // `__STDC__`). Before, `defined()` consulted ONLY table_, so a
        // `#if defined(_WIN32)` OS-selection guard could never see the predefined
        // `_WIN32` (it expands to `1` in a VALUE context but read as undefined in
        // a `defined()` context — the two must agree). predefined_ already
        // reflects the per-format availability filter, so a format-gated macro is
        // `defined` only on its target format.
        // TF-C86 (D-CSUBSET-STDARG-F001A): + the language's conditional-inclusion
        // OPERATORS (`__has_include` & siblings). They are implementation-owned
        // identifiers this preprocessor IMPLEMENTS, so `#ifdef __has_include` is
        // TRUE — MEASURED to match clang. Reading them undefined made the
        // universal `#ifndef __has_include / #define __has_include(x) 0` shim
        // LIVE, which shadowed the real operator with a function-like macro and
        // cascaded into F001A on headers that were present all along. The SAME
        // predicate backs `SynthBuilder::sbNameDefined`, so the two oracles
        // cannot drift.
        return table_.find(std::string{name}) != table_.end()
            || predefined_.find(std::string{name}) != predefined_.end()
            || isConditionalInclusionOperator(name, cfg());
    }

    // The token-text accessor + the macro-state callbacks the shared `sbHandle*`
    // free functions need, bound to THIS expander's buffer + macro table. A
    // `Token` is a 16B POD that does not carry its text, so `textOf` slices it.
    [[nodiscard]] std::function<std::string_view(Token const&)> textOfCb() {
        return [this](Token const& t) { return text(t); };
    }
    [[nodiscard]] std::function<bool(std::string_view)> isDefinedCb() {
        return [this](std::string_view n) { return isDefined(n); };
    }
    [[nodiscard]] std::function<bool(std::vector<Token> const&, std::size_t,
                                     std::size_t)>
    evalExprCb() {
        return [this](std::vector<Token> const& in, std::size_t p,
                      std::size_t end) { return evalIfOperand(in, p, end); };
    }

    // FC14 / c17 (SHARED single-impl): the four conditional-directive handlers
    // delegate to the anon-namespace `sbHandle*` free functions (which the
    // SynthBuilder pre-scan ALSO drives), binding this expander's macro table +
    // buffer via the callbacks above. The `#if EXPR` value is `evalIfOperand`
    // (the full ICE engine over `table_`); `#ifdef`/`#ifndef` definedness is
    // `isDefined`. The diagnostics route to `synth_->id()` exactly as before.
    void handleIf(std::vector<Token> const& in, std::size_t p, std::size_t end,
                  IfKind kind) {
        sbHandleIf(condStack_, in, p, end, kind, textOfCb(), isDefinedCb(),
                   evalExprCb(), rep_, synth_->id());
    }
    void handleElif(std::vector<Token> const& in, std::size_t p,
                    std::size_t end, IfKind kind) {
        // `kind` selects the condition source: Expr -> the `#if` evaluator;
        // Ifdef/Ifndef -> the DIRECT bare-name definedness (C23 elifdef/elifndef,
        // C 6.10.1p5). The definedness callbacks are the SAME ones handleIf binds
        // for `#ifdef`/`#ifndef`, so the two agree on what "defined" means.
        sbHandleElif(condStack_, in, p, end, kind,
                     (p <= end && p > 0 ? in[p - 1].span : SourceSpan::empty(0)),
                     textOfCb(), isDefinedCb(), evalExprCb(), rep_, synth_->id());
    }
    void handleElse(SourceSpan at) {
        sbHandleElse(condStack_, at, rep_, synth_->id());
    }
    void handleEndif(SourceSpan at) {
        sbHandleEndif(condStack_, at, rep_, synth_->id());
    }

    // Evaluate an `#if`/`#elif` controlling expression: slice the operand tokens
    // (from `p` to the line's newline), then delegate to the shared ICE
    // evaluator (`pp_if_eval`), which reuses the const-eval arithmetic core +
    // the existing macro expander (via the callbacks below). Returns the
    // BRANCH-TAKEN boolean (the evaluator already emitted any fail-loud
    // diagnostic; a nullopt -> false, the branch is not taken).
    [[nodiscard]] bool evalIfOperand(std::vector<Token> const& in,
                                     std::size_t p, std::size_t end) {
        // The operand runs from `p` up to (but not including) the trailing
        // newline that `lineEnd` consumed.
        std::size_t last = end;
        while (last > p && isNewline(in[last - 1])) --last;
        std::vector<Token> operand(in.begin() + static_cast<std::ptrdiff_t>(p),
                                   in.begin() + static_cast<std::ptrdiff_t>(last));
        PpMacroExpand expandCb =
            [this](std::vector<Token> const& toks) { return expandTokens(toks); };
        PpIsDefined definedCb =
            [this](std::string_view n) { return isDefined(n); };
        // FC15c + D-INCLUDE-ANGLE-SOURCE-FALLBACK: `__has_include` resolves a
        // header EXACTLY as the include machinery would. This is the AUTHORITATIVE
        // pass's callback (it decides the FINAL `#if` branch); it MUST agree with
        // the SynthBuilder pre-scan callback AND the angle `#include <h>` arm, so
        // all three route through the SAME `resolveAngleInclude` funnel — a
        // descriptor / source / miss verdict that can never drift. Quote form =
        // self-dir + includeDirs (`resolveIncludePath`), unchanged.
        PpHasInclude hasIncludeCb =
            [this](std::string_view filename, bool isAngle) -> bool {
            if (isAngle) {
                // Descriptor -> per-target availability (c9: an unavailable-on-this-
                // format header reports NOT available, agreeing with the `#include`
                // gate + macro-splice; nullopt activeFormat_ = pure existence).
                // Source -> a real header on the -I path is includable (the arm
                // textually splices it) -> 1. NotFound -> 0.
                //   D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: a `__has_include(<h>)` probe
                //   does NOT seed -- the un-gated angle-`#include` splice is the SOLE
                //   recorder, so the seed set stays == the finish() oracle's live set.
                AngleIncludeResolution const ar =
                    resolveAngleInclude(filename, systemDirs_, includeDirs_,
                                        headerNameMatching_);
                switch (ar.kind) {
                    case AngleIncludeKind::Descriptor:
                        return !(activeFormat_.has_value()
                                 && !ffi::shippedHeaderAvailableForFormat(
                                        ar.path, *activeFormat_));
                    case AngleIncludeKind::Source:
                        return true;
                    case AngleIncludeKind::NotFound:
                        return false;
                    case AngleIncludeKind::AmbiguousDescriptor:
                    case AngleIncludeKind::AmbiguousSource:
                        // D-PP-HEADER-CASE-INSENSITIVE-PE: this is the
                        // AUTHORITATIVE evaluation of a LIVE `#if` operand, so
                        // reporting here is both allowed and REQUIRED — a
                        // `__has_include`-guarded include is the one shape whose
                        // collision no later tier would ever see (answer 0, the
                        // `#include` never materializes, nothing else resolves
                        // that name). Answering 0 SILENTLY was the silent-drop.
                        // BOTH halves report here: unlike the splice arm, the
                        // operator's answer is this tier's alone.
                        reportHeaderCaseAmbiguity(rep_, BufferId{},
                                                  SourceSpan::empty(0), filename,
                                                  ar.ambiguousCandidates);
                        return false;
                }
                return false;   // unreachable — every AngleIncludeKind handled above
            }
            HeaderSearchResult const q =
                resolveIncludePath(filename, includingDir_, includeDirs_,
                                   headerNameMatching_);
            if (q.status == HeaderSearchStatus::AmbiguousCase) {
                reportHeaderCaseAmbiguity(rep_, BufferId{}, SourceSpan::empty(0),
                                          filename, q.ambiguousCandidates);
                return false;
            }
            return q.status == HeaderSearchStatus::Found;
        };
        // FC15b: surface the accumulated product tail (a predefined/`#`/`##`
        // product expanded inside this `#if` operand materializes into it) so the
        // evaluator assembles a combined prefix+product buffer to slice it.
        PpProductText productCb =
            [this]() -> std::string_view { return productText_; };
        // FC17.9(h): `__has_embed` answers EXACTLY what `#embed` would do at the
        // operator's spot -- per-origin resolution (the dir of the file containing
        // the operator, derived from `opSpan` via the line-map), then the C23
        // trichotomy NOT_FOUND(0) / FOUND(1) / EMPTY(2). Angle form -> 0 (the
        // deferred angle form resolves no binary resource). Uses the SAME
        // resolveIncludePath + is_regular_file the directive uses, so the operator
        // and the directive can never disagree on a resource's existence/size.
        PpHasEmbed embedCb =
            [this](std::string_view filename, bool isAngle,
                   SourceSpan opSpan) -> int {
            if (isAngle) return 0;   // D-PP-EMBED-ANGLE: nothing to resolve
            HeaderSearchResult const r =
                resolveIncludePath(filename, embedResolutionDir(opSpan),
                                   includeDirs_, headerNameMatching_);
            if (r.status == HeaderSearchStatus::AmbiguousCase) {
                // Authoritative + live (same argument as `__has_include`
                // above): a `__has_embed`-guarded resource whose name
                // fold-collides is seen by NO other tier.
                reportHeaderCaseAmbiguity(rep_, BufferId{}, SourceSpan::empty(0),
                                          filename, r.ambiguousCandidates);
                return 0;
            }
            if (r.status != HeaderSearchStatus::Found) return 0;  // NOT_FOUND
            auto const& resolved = r.path;
            std::error_code ec;
            if (!fs::is_regular_file(resolved, ec)) return 0;   // NOT_FOUND
            auto const sz = fs::file_size(resolved, ec);
            if (ec) return 0;                                   // stat failed
            return sz == 0 ? 2 /*EMPTY*/ : 1 /*FOUND*/;
        };
        auto v = evaluateIfExpression(operand, *schema_, expandCb, definedCb,
                                      hasIncludeCb, *synth_, productCb, rep_,
                                      embedCb);
        return v.has_value() && *v;
    }

    // Macro-expand a token run with the SAME engine `run()` uses (object +
    // function-like, hide-set-precise): lift into the ExpToken working set,
    // expand, drop the hide sets. Used by the `#if` evaluator's callback so the
    // controlling expression's macros expand identically to the body's.
    std::vector<Token> expandTokens(std::vector<Token> const& toks) {
        // D-PP-DEFINED-VIA-MACRO-EXPANSION: this is the ONLY entry point that
        // expands a `#if`/`#elif` CONTROLLING EXPRESSION (the `run()` body pass
        // has its own), so the `defined` operand barrier is armed HERE and
        // nowhere else — an ordinary body token spelled `defined` stays an
        // ordinary identifier. Armed for the duration of ONE evaluation and torn
        // down after it, so a barrier can never leak into the body pass.
        PpIfOperandBarrier barrier{*schema_};
        struct ArmedBarrier {
            PpIfOperandBarrier** slot;
            explicit ArmedBarrier(PpIfOperandBarrier** s, PpIfOperandBarrier* b)
                : slot(s) { *slot = b; }
            ~ArmedBarrier() { *slot = nullptr; }
            ArmedBarrier(ArmedBarrier const&)            = delete;
            ArmedBarrier& operator=(ArmedBarrier const&) = delete;
        } const armed{&ifDefinedBarrier_, &barrier};
        // FC15b: seed each token's own offset as its invocation anchor (a
        // `__LINE__` in a `#if` operand resolves against that operand's line).
        // `liftRun` additionally stamps `spacedBefore` from this run's trivia, so a
        // `#`-stringize reached from a `#if` operand spells the same as in the body.
        std::vector<ExpToken> expanded = expand(liftRun(toks), 0);
        std::vector<Token> out;
        out.reserve(expanded.size());
        // FC15 paste residuals: backstop drop of any stray placemarker (see
        // `run()`); the primary drop is at `collapsePastes` return.
        for (ExpToken const& et : expanded) {
            if (et.placemarker) continue;
            out.push_back(et.tok);
        }
        return out;
    }

    bool isParenOpen(Token const& t) const {
        return parenOpen_.valid() && t.schemaKind == parenOpen_;
    }
    bool isParenClose(Token const& t) const {
        return parenClose_.valid() && t.schemaKind == parenClose_;
    }
    bool isArgSeparator(Token const& t) const {
        return argSep_.valid() && t.schemaKind == argSep_;
    }
    bool isVariadicMarker(Token const& t) const {
        return variadicMarker_.valid() && t.schemaKind == variadicMarker_;
    }

    // FC17.9(h): read `path`'s bytes BINARY-exact into a string, WITHOUT minting a
    // BufferId / registering a source (the byte-body of `SourceBuffer::fromFile`
    // without the buffer machinery, and without throwing). nullopt on an open or
    // read failure (the caller emits the loud unreadable diagnostic).
    // `std::ios::binary` is load-bearing on Windows -- a CR/LF/SUB byte in the
    // resource must survive verbatim (pinned by tests).
    static std::optional<std::string> readResourceBytes(fs::path const& path) {
        // The shared `resolveIncludePath` matches any directory ENTRY, so it can
        // hand back a DIRECTORY. Require a regular file (nullopt -> the caller's loud
        // unreadable diagnostic) so a directory-named resource fails LOUD, never
        // reads as a silently-empty embed.
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return std::nullopt;
        // A mid-stream IO error (disk/share failure) can silently truncate the
        // read, and a truncated `#embed` is a SILENT MISCOMPILE — the program
        // gets fewer bytes than the resource holds and nothing says so.
        // THE ONE CHECKED READ
        // (D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT).
        // ⚠ The `if (in.bad())` this replaces could not fire: `<< in.rdbuf()`
        // inserts through the STREAMBUF and never touches the istream object's
        // state (✔MEASURED), so this resource read has been unguarded since it
        // was written. The helper compares BYTES against the size it measured,
        // which is the only detector a short read has.
        auto text = core::readFileChecked(path);
        if (!text) return std::nullopt;
        return *std::move(text);
    }

    // FC17.9(h): the directory the `#embed` quote form resolves against -- the
    // directory of the FILE that CONTAINS the directive (C23: the quote search is
    // "as for #include" = relative to the containing file). Derive it from the
    // directive word's offset via the line-map ORIGIN (the `__FILE__` File-kind
    // precedent), so an `#embed` spliced in from a quote-header resolves relative
    // to THAT header. Null/empty line-map or a null origin -> the main file's dir.
    fs::path embedResolutionDir(SourceSpan dirSpan) const {
        if (lineMap_ != nullptr && !lineMap_->empty()) {
            LineMap::Resolved const r = lineMap_->resolve(dirSpan.start());
            if (r.origin != nullptr) {
                // The SHARED derivation
                // (D-PP-BARE-RELATIVE-MAIN-PATH-DEFEATS-THE-INCLUDER-DIRECTORY-SEARCH):
                // an origin named without a directory component resolves its
                // `#embed` against the working directory, exactly as a quote
                // `#include` in the same file does.
                return includingDirectoryOf(r.origin->name());
            }
        }
        return includingDir_;
    }

    // FC17.9(h) C23 `#embed` (6.10.4 / N3096 6.10.3): the directive handler
    // (D-PP-EMBED). Splices the QUOTED resource's bytes into `body` as a
    // comma-separated list of decimal `int` constants (0..255), via the SAME
    // product-token mechanism (`materializeSignificant`) the `#`/`##` operators
    // use -- the spliced tokens are ordinary IntLiteral/Comma tokens that survive
    // expansion untouched (only Word tokens re-trigger macros) and the parser
    // accepts in a brace initializer. `wordIdx` is the `embed` directive WORD (the
    // anchor for every diagnostic + the per-origin resolution dir). Every
    // non-bare-quote-filename shape and every unsupported construct fails LOUD with
    // `P_PreprocessorEmbed` -- never a silent drop, never a silent partial embed.
    void handleEmbed(std::vector<Token> const& in, std::size_t wordIdx,
                     std::size_t end, std::vector<Token>& body) {
        SourceSpan const dirSpan = in[wordIdx].span;   // the `embed` word
        std::size_t p = skipTrivia(in, wordIdx + 1);

        // ── Extract the quote filename (the `#include "h"` shape). ──
        if (p >= end || isNewline(in[p])) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorEmbed, synth_->id(),
                   dirSpan, "#embed requires a \"resource\" filename");
            return;
        }
        // An angle opener (LtOp, the reused hasIncludeAngle kind) -> the deferred
        // angle form (D-PP-EMBED-ANGLE: DSS ships JSON descriptors, not binary
        // resources, on the system path). Anything that is NOT the quote opener
        // (e.g. a macro name -- C23's "expand if not one of the forms") -> the
        // deferred macro-argument form (D-PP-EMBED-MACRO-ARG). Never silent.
        if (embedAngleOpenKind_.valid()
            && in[p].schemaKind == embedAngleOpenKind_) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorEmbed, synth_->id(),
                   dirSpan,
                   "#embed <resource> (angle form) is not supported "
                   "(D-PP-EMBED-ANGLE); use \"resource\"");
            return;
        }
        if (!quoteIncludeKind_.valid() || in[p].schemaKind != quoteIncludeKind_) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorEmbed, synth_->id(),
                   dirSpan,
                   "#embed requires a \"resource\" filename (a macro-expanded "
                   "argument is not supported: D-PP-EMBED-MACRO-ARG)");
            return;
        }
        // The quote opener consumed only the opening `"`; the coalesced string
        // BODY is the ADJACENT next token, its raw text the filename (escapes NOT
        // decoded, like the include resolver). An empty body (`#embed ""`) leaves
        // the filename empty -> loud below. The closing `"` is its OWN token
        // (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN), so `after` must step past BOTH
        // the body and the closer — otherwise the parameter wall right below sees
        // the delimiter and reports `parameter '"' is not supported`, rejecting
        // every well-formed `#embed "res.bin"`.
        std::string filename;
        std::size_t after = p + 1;   // token index just past the filename body
        if (after < end && !isTrivia(in[after]) && !isNewline(in[after])
            && in[after].span.start() == in[p].span.end()) {
            filename = std::string{text(in[after])};
            after    = pastBodyAndCloser(in, after);
        }
        if (filename.empty()) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorEmbed, synth_->id(),
                   dirSpan, "#embed has an empty resource filename");
            return;
        }

        // ── Reject parameters loudly (D-PP-EMBED-PARAMS). ANY significant token
        // after the filename before the line-end newline -> loud, naming it.
        // Silently honoring `limit(N)`/`prefix`/`suffix`/`if_empty`/vendor would
        // embed a different byte set than the program asked for -- a silent
        // miscompile class; the loud wall is the VLA-C1a fail-loud precedent. ──
        std::size_t q = skipTrivia(in, after);
        if (q < end && !isNewline(in[q])) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorEmbed, synth_->id(),
                   dirSpan,
                   std::string{"#embed parameter '"} + std::string{text(in[q])}
                       + "' is not supported (D-PP-EMBED-PARAMS); only "
                         "`#embed \"resource\"` is supported this cycle");
            return;
        }

        // ── Resolve the resource EXACTLY as a quote-`#include` would (the ONE
        // shared quote search: absolute -> direct; else self-dir first, then the
        // include dirs), relative to the FILE that contains the directive. ──
        HeaderSearchResult const rr =
            resolveIncludePath(filename, embedResolutionDir(dirSpan),
                               includeDirs_, headerNameMatching_);
        if (rr.status == HeaderSearchStatus::AmbiguousCase) {
            reportHeaderCaseAmbiguity(rep_, synth_->id(), dirSpan, filename,
                                      rr.ambiguousCandidates);
            return;
        }
        if (rr.status != HeaderSearchStatus::Found) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorEmbed, synth_->id(),
                   dirSpan, std::string{"#embed resource not found: "} + filename);
            return;
        }
        auto const* resolved = &rr.path;

        // ── Read the bytes BINARY-exact (CRLF/SUB/NUL/0xFF preserved). ──
        auto bytes = readResourceBytes(*resolved);
        if (!bytes) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorEmbed, synth_->id(),
                   dirSpan, std::string{"#embed resource could not be read: "}
                                + resolved->string());
            return;
        }

        // ── FIX-1 (D-PP-EMBED, the streaming boundary): gate the byte COUNT
        // through the pure size helper -- a catchable LOUD wall, never an OOM. ──
        if (auto sizeErr = embedResourceSizeError(bytes->size())) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorEmbed, synth_->id(),
                   dirSpan, std::move(*sizeErr));
            return;
        }

        // ── Empty resource -> empty expansion (C23 6.10.3/6.10.4: the byte list
        // is empty; cycle-1 has no `if_empty` parameter to change that). Push
        // nothing and return -- the directive expands to the empty sequence. ──
        if (bytes->empty()) return;

        // ── Splice: spell the bytes as a comma-separated decimal `int` list, then
        // materialize the product tokens (IntLiteral/Comma) and push into `body`
        // (the include-arm push point). A `'\n'` before every 16th element keeps
        // any later diagnostic-rendered product line short; `materializeSignificant`
        // DROPS trivia/newlines from its returned tokens, so the newlines cost
        // zero tokens (the spliced stream is exactly `42, 13, 10, ...`). ──
        std::string spelling;
        spelling.reserve(bytes->size() * 5);
        for (std::size_t bi = 0; bi < bytes->size(); ++bi) {
            if (bi != 0) {
                spelling.push_back(',');
                spelling.push_back((bi % 16 == 0) ? '\n' : ' ');
            }
            spelling.append(std::to_string(static_cast<unsigned>(
                static_cast<unsigned char>((*bytes)[bi]))));
        }
        // [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: `#embed`'s byte list is
        // minted through the SAME chokepoint as a `#`/`##` product, so it lands
        // past the prefix and used to extrapolate exactly the same way. Its
        // expansion site is the DIRECTIVE — there is no macro and no `#define`
        // line, so a diagnostic on a spliced byte points at the `#embed` that
        // put it there. ★ This arm is why the provenance is stamped at
        // `materializeSignificant` rather than in `expand`: `#embed` never goes
        // through a macro invocation at all.
        MintScope const mintScope{*this, dirSpan.start(), 0, /*hasDef=*/false,
                                  std::string{"#"} + cfg().embedDirective};
        for (Token const& t : materializeSignificant(spelling)) {
            body.push_back(t);
        }
    }

    void handleDefine(std::vector<Token> const& in, std::size_t p,
                      std::size_t end) {
        const char space = static_cast<char>(0x20);
        p = skipTrivia(in, p);
        if (p >= end || isNewline(in[p]) || !isWord(in[p])) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   (p < end ? in[p].span : SourceSpan::empty(0)),
                   "#define requires a macro name");
            return;
        }
        const std::string name{text(in[p])};
        const std::size_t nameIdx = p;
        ++p;

        // FC15b (C23 6.10.10.1p2): a `#define` of a name this implementation
        // predefined. Looked up in the config-seeded set, so the engine never
        // hard-codes a name.
        //
        // ★ D-PP-PREDEFINE-REDEFINITION-PARTITION — WHAT THIS SET NOW IS.
        // `predefined_` holds exactly the rows whose config `programRedefinition`
        // is a WARN verb. An implementation-supplied extension — a compiler- or
        // arch-identity row, whatever the config names it — is no longer in it: it
        // lowers to an ordinary "<built-in>" `#define`, so it never reaches this
        // arm and only the ordinary 6.10.5p2 policy applies — which is the
        // reference behaviour, MEASURED on gcc 13.3.0 and clang 18.1.3
        // SEPARATELY over a 36-name sweep. ⚠ SO THIS ARM IS NO LONGER "the
        // predefined guard": it is the DIAGNOSED half of a partition, and
        // widening `predefined_` back to every config row would make DSS warn
        // about an `#undef` of an ordinary identity macro, which neither
        // reference does.
        //
        // ★★ IT IS A WARNING AND IT FALLS THROUGH — AND THAT REVERSAL IS THE
        // POINT OF THE ROW. It used to be an ERROR with an early `return`, on
        // the reading that 6.10.10.1p2 is a CONSTRAINT. ✔RE-MEASURED against
        // N3220: 6.10.10 carries no `Constraints` heading (6.10.5 does), so
        // C23 4p2 makes the violation UNDEFINED BEHAVIOUR, and 5.1.1.3 requires
        // a diagnostic only for a syntax-rule or CONSTRAINT violation. Nothing
        // is required here at all. ✔MEASURED on gcc 13.3.0 and clang 18.1.3
        // separately: both WARN and both APPLY — `#undef __STDC__` then
        // `#define __STDC__ 77` prints 77 on both, and `#undef __LINE__` then
        // `int __LINE__ = 9;` compiles on both. So refusing was a divergence,
        // and it is the same shape as D-PP-INCOMPATIBLE-REDEFINITION-IS-FATAL
        // one tier up: being stricter than every reference is not rigor.
        // ⚠ The `erase` is load-bearing, not cleanup. It is what makes the
        // directive TAKE EFFECT: `expand` materializes a predefined value only
        // on a `table_` MISS, and `isDefined` ORs the two maps, so a name left
        // in `predefined_` would keep expanding and keep reading `#ifdef`-true
        // no matter what the program wrote. Erasing also makes the FIRST
        // program action consume the name's diagnosed status — matching clang,
        // where a second `#undef`/`#define` cycle is silent.
        if (auto pit = predefined_.find(name); pit != predefined_.end()) {
            if (predefinedNameIsDiagnosedOnChange(
                    pit->second.programRedefinition)) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorPredefinedMacro,
                       synth_->id(), in[nameIdx].span,
                       std::string{"'"} + name
                           + "' is a predefined macro of this implementation; "
                             "#defining it replaces the implementation's value",
                       DiagnosticSeverity::Warning);
            }
            predefined_.erase(pit);
        }

        // TF-C86 (D-CSUBSET-STDARG-F001A): the sibling constraint for the
        // CONDITIONAL-INCLUSION OPERATORS (C23 6.10.1). Same posture as the
        // predefined arm above and the same reason at a different tier: honoring
        // `#define __has_include(x) 0` would let the guard answer 0 while
        // `#include <h>` still splices the header — one program, two verdicts on
        // one file. Config-driven name set, no hard-coded spelling.
        if (isConditionalInclusionOperator(name, cfg())) {
            emitPP(rep_,
                   DiagnosticCode::P_PreprocessorOperatorNameNotDefinable,
                   synth_->id(), in[nameIdx].span,
                   std::string{"'"} + name
                       + "' is a conditional-inclusion operator this "
                         "implementation provides and may not be #defined");
            return;
        }

        // FC18a (D-PP-VA-OPT, C23 6.10.5p5): `__VA_OPT__` is not an ordinary
        // identifier -- it may occur ONLY as the introducer of a
        // va-opt-replacement inside a variadic macro's replacement list, so it can
        // never be a macro NAME. Same posture as the two guards above and the same
        // reason: honoring the `#define` would shadow a construct the engine
        // implements, leaving one spelling with two meanings. ✔MEASURED: cl 19.51
        // answers C4117 ("reserved, '#define' ignored") and clang-18/clang-19/
        // gcc-13 all reject it under -pedantic-errors. Config-driven name, no
        // hard-coded spelling.
        if (!cfg().vaOptName.empty() && name == cfg().vaOptName) {
            emitPP(rep_,
                   DiagnosticCode::P_PreprocessorOperatorNameNotDefinable,
                   synth_->id(), in[nameIdx].span,
                   std::string{"'"} + name
                       + "' is a variadic-macro operator this implementation "
                         "provides and may not be #defined");
            return;
        }

        MacroDef def;
        // [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: the macro's own NAME token is
        // its definition site — where "expanded from macro 'X'" points. Taken
        // here, on the ONE path that builds a def from a directive line, so it is
        // recorded for every macro rather than for the ones someone remembered.
        // NOT part of `sameDefinition`: two `#define`s of the same macro on
        // different lines are the SAME definition (C 6.10.3p2 compares the
        // replacement list), and making the position part of identity would turn
        // a benign repeat into a redefinition error.
        def.definitionSite    = in[nameIdx].span.start();
        def.hasDefinitionSite = true;
        // FUNCTION-like iff the configured open-paren is IMMEDIATELY ADJACENT
        // to the macro name (C 6.10.3p3: no white space between the name and
        // the `(`). `#define F (x)` -- a space before `(` -- is OBJECT-like
        // (the `(x)` is part of the replacement list). We test adjacency on the
        // raw spans (NO skipTrivia) so a single intervening space disqualifies.
        if (p < end && isParenOpen(in[p])
            && in[p].span.start() == in[nameIdx].span.end()) {
            def.isFunctionLike = true;
            if (!parseParamList(in, p, end, name, def.params, def.isVariadic)) {
                return;  // parseParamList already emitted a fail-loud diagnostic
            }
            // After the parameter list, `p` indexes the token just past `)`;
            // the rest of the line is the replacement list (collected below).
        }

        std::string repText;
        std::string repSpacing;
        // The source END offset of the previous replacement token, so the gap to
        // the next one answers "was there white space between them" WITHOUT
        // re-scanning trivia (`skipTrivia` has already stepped over it, and a
        // COMMENT is white space for this purpose exactly as C 6.10.3p2 says).
        ByteOffset prevEnd = 0;
        for (std::size_t q = skipTrivia(in, p); q < end;
             q = skipTrivia(in, q + 1)) {
            if (isNewline(in[q])) break;
            if (!repText.empty()) repText.push_back(space);
            // First token: no PREVIOUS token to be separated from, so '0' by
            // construction -- the leading white space between `#define NAME` and
            // the replacement list is not part of the list (C 6.10.3p2).
            repSpacing.push_back(
                (!def.replacement.empty() && in[q].span.start() != prevEnd) ? '1' : '0');
            def.replacement.push_back(in[q]);
            repText.append(text(in[q]));
            prevEnd = in[q].span.end();
        }
        def.text    = std::move(repText);
        def.spacing = std::move(repSpacing);

        // The variadic catch-all identifier (`__VA_ARGS__`) is valid ONLY inside
        // a VARIADIC macro's replacement (C 6.10.3p5 / 6.10.3.1p2 constraint:
        // the identifier `__VA_ARGS__` shall occur only in the replacement-list
        // of a function-like macro that uses the ellipsis notation). Reject it
        // HERE, at definition time, in an object-like OR a non-variadic
        // function-like macro -- catching the misuse where it is DECLARED rather
        // than waiting for a (possibly absent) invocation. Matched by TEXT (it is
        // an ordinary identifier), and only when the language actually declares a
        // catch-all spelling (`variadicArgsName` non-empty).
        if (!def.isVariadic && !cfg().variadicArgsName.empty()) {
            for (Token const& r : def.replacement) {
                if (isWord(r) && text(r) == cfg().variadicArgsName) {
                    emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                           synth_->id(), r.span,
                           std::string{"'"} + cfg().variadicArgsName
                               + "' may appear only in a variadic macro's "
                                 "replacement: " + name);
                    return;
                }
            }
        }

        // FC18a (D-PP-VA-OPT): every va-opt-replacement constraint, checked HERE
        // at definition time -- where the construct is WRITTEN, rather than at a
        // possibly-absent invocation. `validateVaOpt` emits its own diagnostic.
        if (!validateVaOpt(def, name)) return;

        // D-PP-INCOMPATIBLE-REDEFINITION-IS-FATAL: an incompatible redefinition is
        // a C 6.10.3p2 CONSTRAINT VIOLATION, so a diagnostic is REQUIRED -- but it
        // is a WARNING and translation CONTINUES with the NEW definition, because
        // that is what the compilers this language declares itself to be actually
        // do. ✔MEASURED across every divergence shape `sameDefinition` can report,
        // one TU each, `-std=c2x -pedantic` (and cross-checked with `cl /Zs`):
        //
        //   shape                | reference        | definition in effect after
        //   parameter spelling   | warning, rc=0    | the SECOND (new) one
        //   replacement text     | warning, rc=0    | the SECOND (new) one
        //   arity                | warning, rc=0    | the SECOND (new) one
        //   object vs fn-like    | warning, rc=0    | the SECOND (new) one
        //   variadic vs not      | warning, rc=0    | the SECOND (new) one
        //
        // Uniform: every shape warns, none is fatal, and the new definition always
        // wins. Only `-Werror` makes any of them stop a build, which is the user's
        // choice about warnings, not a property of the construct. ★ The shapes were
        // measured INDIVIDUALLY on purpose -- generalizing from the one that bit us
        // (parameter spelling) would have been a guess wearing a measurement's
        // clothes, and the sweep is also what caught the OPPOSITE defect recorded
        // on `MacroDef::spacing`, where DSS was silently accepting a divergence the
        // references diagnose.
        //
        // ⚠ WHY THIS IS A CONFORMANCE FIX AND NOT A RELAXATION: the standing rule is
        // that the reference compilers are the spec, BIDIRECTIONALLY -- DSS may not
        // reject what they accept, and may not silently accept what they diagnose.
        // Being stricter than every reference is not rigor; it is a divergence that
        // makes real code unbuildable. ✔The live consumer: sqlite's `shell.c.in`
        // defines `S_ISLNK(mode) (0)` BEFORE it includes <sys/stat.h>, so the
        // shipped descriptor's own macro lands on top of it -- fatal here, fine on
        // gcc/clang/MSVC. Matching a descriptor's parameter SPELLING to whichever
        // consumer is in front of us today is a lottery, not a fix.
        auto it = table_.find(name);
        if (it != table_.end() && !sameDefinition(it->second, def)) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorMacroRedefinition,
                   synth_->id(), in[nameIdx].span,
                   std::string{"incompatible redefinition of macro: "} + name,
                   DiagnosticSeverity::Warning);
            // NO `return` -- falling through is the load-bearing half. The old code
            // both errored AND kept the OLD definition; the references keep the NEW
            // one, so an early return here would leave DSS wrong about the VALUE
            // even once the severity matched.
        }
        table_[name] = std::move(def);
    }

    // FC18a (D-PP-VA-OPT): enforce every C23 va-opt-replacement constraint on a
    // freshly parsed `#define`. Returns false (having emitted ONE diagnostic)
    // when the definition must be rejected; true when it is well formed or the
    // language declares no va-opt construct at all.
    //
    // The constraints, each named where it is checked below:
    //   6.10.5p5     -- may occur ONLY in a variadic function-like macro's
    //                   replacement list (so: not object-like, not non-variadic).
    //   6.10.5.1p3   -- must occur as `__VA_OPT__ ( pp-tokens_opt )`: a `(` must
    //                   follow, and its matching `)` must exist.
    //   6.10.5.1p3   -- the content shall NOT contain another `__VA_OPT__`.
    //   6.10.5.1p3   -- the content "shall form a valid replacement list", which
    //                   via 6.10.5.3p1 forbids a `##` at either END of it. This is
    //                   the standard's own H1 example.
    //
    // ★ THIS IS THE DIAGNOSIS FIX, NOT JUST A FEATURE GATE. Before FC18a a
    // `__VA_OPT__` reached the PARSER as an unknown identifier and the user was
    // told `expected 'ParenClose'` -- a true statement about the parser's stack
    // that named neither the construct nor the reason. Every arm below names
    // `__VA_OPT__` and the rule it broke.
    [[nodiscard]] bool validateVaOpt(MacroDef const& def,
                                     std::string const& macroName) {
        if (cfg().vaOptName.empty()) return true;   // language has no va-opt
        const std::size_t n = def.replacement.size();
        for (std::size_t i = 0; i < n; ++i) {
            Token const& r = def.replacement[i];
            if (!isVaOptWord(r)) continue;
            // 6.10.5p5: only inside a VARIADIC function-like macro.
            if (!def.isVariadic) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), r.span,
                       std::string{"'"} + cfg().vaOptName
                           + "' may appear only in a variadic macro's "
                             "replacement: " + macroName);
                return false;
            }
            std::size_t open = 0;
            const std::size_t close = findVaOptClose(def.replacement, i, open);
            if (close == vaOptNpos()) {
                // Distinguish "no `(` at all" from "`(` never closed" -- they are
                // different mistakes and the user fixes them differently.
                const bool hasOpen =
                    i + 1 < n && isParenOpen(def.replacement[i + 1]);
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), r.span,
                       std::string{"'"} + cfg().vaOptName
                           + (hasOpen
                                  ? "' is missing the ')' that closes its "
                                    "replacement content"
                                  : "' must be followed by '(' -- it is only "
                                    "valid as the form '"
                                        + cfg().vaOptName + "( content )'"));
                return false;
            }
            // 6.10.5.1p3: the content shall not contain another va-opt.
            for (std::size_t j = open + 1; j < close; ++j) {
                if (isVaOptWord(def.replacement[j])) {
                    emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                           synth_->id(), def.replacement[j].span,
                           std::string{"'"} + cfg().vaOptName
                               + "' must not be nested inside another '"
                               + cfg().vaOptName + "'");
                    return false;
                }
            }
            // 6.10.5.1p3 + 6.10.5.3p1: the content must be a valid replacement
            // list, so no `##` at either end of it. (An EMPTY content is fine --
            // `__VA_OPT__()` is a well-formed no-op.)
            if (close > open + 1) {
                const bool pasteAtStart = isPaste(def.replacement[open + 1]);
                const bool pasteAtEnd   = isPaste(def.replacement[close - 1]);
                if (pasteAtStart || pasteAtEnd) {
                    emitPP(rep_, DiagnosticCode::P_PreprocessorPaste,
                           synth_->id(),
                           (pasteAtStart ? def.replacement[open + 1].span
                                         : def.replacement[close - 1].span),
                           std::string{"'##' must not appear at the "}
                               + (pasteAtStart ? "start" : "end") + " of a '"
                               + cfg().vaOptName + "' replacement content");
                    return false;
                }
            }
            i = close;   // skip the whole construct; nesting was rejected above
        }
        return true;
    }

    // Two `#define`s of the same name are COMPATIBLE (C 6.10.3p1/p2) only when
    // they agree on EVERY axis: object-vs-function-like, the parameter spelling
    // (in order), AND the replacement-token spelling. We compare the
    // whitespace-normalized replacement TEXT (the cycle-1 contract) plus the
    // kind + parameter names. A mismatch on any axis is an incompatible
    // redefinition (fail-loud at the call site).
    static bool sameDefinition(MacroDef const& a, MacroDef const& b) {
        return a.isFunctionLike == b.isFunctionLike
            && a.isVariadic == b.isVariadic
            && a.params == b.params
            && a.text == b.text
            // C 6.10.3p2's white-space clause -- see `MacroDef::spacing`. Without
            // this term the space-joined `text` reports `40+2` and `40 + 2` as one
            // definition, and DSS accepts SILENTLY what both references diagnose.
            && a.spacing == b.spacing;
    }

    // Parse a function-like macro's parameter list, starting at `open` (the
    // index of the adjacent `(`). On success fills `out` with the NAMED
    // parameter names in order, sets `isVariadic` iff a trailing `...` catch-all
    // is present, advances `open` PAST the closing `)`, and returns true. On any
    // malformed input emits a fail-loud diagnostic and returns false.
    // Grammar (C 6.10.3, FC13 cycle 3 -- plain variadic, no `#`/`##`):
    //   ( )                        -> zero parameters
    //   ( id ( , id )* )           -> named parameters, comma-separated
    //   ( ... )                    -> zero named + a variadic catch-all
    //   ( id ( , id )* , ... )     -> named parameters + a variadic catch-all
    // The `...` (when the language declares one) must be the LAST element before
    // `)`; it is accepted (sets isVariadic), NOT a fail-loud as in cycle 2.
    // FAIL-LOUD on: a `...` that is NOT last (`(a, ..., b)` / a token after the
    // `...` other than `)`), a duplicate parameter name, a non-identifier where a
    // parameter is expected, a missing comma between parameters, or no closing
    // `)` before line end.
    bool parseParamList(std::vector<Token> const& in, std::size_t& open,
                        std::size_t end, std::string const& macroName,
                        std::vector<std::string>& out, bool& isVariadic) {
        std::size_t q = skipTrivia(in, open + 1);  // first token after `(`
        // Empty list `()` -> zero parameters.
        if (q < end && isParenClose(in[q])) {
            open = q + 1;
            return true;
        }
        while (true) {
            if (q >= end || isNewline(in[q])) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(),
                       (q < end ? in[q].span : SourceSpan::empty(0)),
                       std::string{"unterminated macro parameter list: "}
                           + macroName);
                return false;
            }
            // A variadic marker (C's `...`) in parameter position makes this a
            // VARIADIC macro (C 6.10.3p4). Detected by the CONFIGURED token KIND
            // (`variadicMarkerToken`), NOT the hard-coded `...` lexeme: a second
            // preprocess-opting language whose variadic marker is spelled
            // differently is parsed correctly (the `.valid()` guard means a
            // language declaring no variadic form never matches here -- the
            // marker then falls through to the not-a-Word fail-loud below). The
            // `...` must be LAST: the only thing allowed after it is the closing
            // `)`. We reach this arm at the START of the list (`(...)`) and after
            // each comma (`(a, ...)`), so requiring `)` next rejects the mid-list
            // form `(a, ..., b)` and a stray token after `...`.
            if (isVariadicMarker(in[q])) {
                std::size_t r = skipTrivia(in, q + 1);
                if (r < end && isParenClose(in[r])) {
                    isVariadic = true;
                    open = r + 1;
                    return true;
                }
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(),
                       (r < end ? in[r].span : in[q].span),
                       std::string{"variadic '...' must be the last element of "
                                   "the macro parameter list: "}
                           + macroName);
                return false;
            }
            if (!isWord(in[q])) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), in[q].span,
                       std::string{"expected a parameter name in macro "
                                   "parameter list: "}
                           + macroName);
                return false;
            }
            std::string param{text(in[q])};
            // C 6.10.3p6: the configured catch-all identifier (`__VA_ARGS__`)
            // shall NOT be used as a parameter NAME. Reject it loudly so the
            // substitute() invariant ("`__VA_ARGS__` is not a valid parameter
            // name") actually holds -- otherwise `#define F(__VA_ARGS__) ...`
            // silently binds a parameter the variadic catch-all later shadows.
            if (!cfg().variadicArgsName.empty()
                && param == cfg().variadicArgsName) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), in[q].span,
                       std::string{"'"} + cfg().variadicArgsName
                           + "' may not be used as a macro parameter name: "
                           + macroName);
                return false;
            }
            for (std::string const& seen : out) {
                if (seen == param) {
                    emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                           synth_->id(), in[q].span,
                           std::string{"duplicate macro parameter '"} + param
                               + "' in macro: " + macroName);
                    return false;
                }
            }
            out.push_back(std::move(param));
            q = skipTrivia(in, q + 1);  // token after the parameter name
            if (q < end && isParenClose(in[q])) {
                open = q + 1;
                return true;
            }
            if (q >= end || isNewline(in[q]) || !isArgSeparator(in[q])) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(),
                       (q < end ? in[q].span : SourceSpan::empty(0)),
                       std::string{"expected ',' or ')' in macro parameter "
                                   "list: "}
                           + macroName);
                return false;
            }
            q = skipTrivia(in, q + 1);  // token after the comma -> next param
        }
    }

    // TF-C59 C23 6.10.4 (D-CPP-LINE-DIRECTIVE): `#line digits ["file"]` sets the
    // PRESUMED line — and optionally the presumed file name — reported by
    // `__LINE__`/`__FILE__` for the lines that FOLLOW. Records a per-origin entry
    // consumed by `presumedLine`/`presumedFile`.
    //
    // The MACRO-EXPANDED operand form (6.10.4p4 — `#line SOME_MACRO`) is NOT yet
    // handled: such an operand is not a digit sequence, so it hits the fail-loud
    // below rather than being silently mis-numbered. Anchored
    // `D-CPP-LINE-DIRECTIVE-MACRO-OPERAND`; generated C (lemon/bison/flex) always
    // emits the literal-digit form, which is what unblocks sqlite's `parse.c`.
    void handleLine(std::vector<Token> const& in, std::size_t p,
                    std::size_t end) {
        std::size_t const dirTok = p;          // first operand token (span source)
        p = skipTrivia(in, p);
        if (p >= end || isNewline(in[p])) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   (p < end ? in[p].span : SourceSpan::empty(0)),
                   "#line requires a line number");
            return;
        }
        std::string_view const numTx = text(in[p]);
        if (numTx.empty()
            || numTx.find_first_not_of("0123456789") != std::string_view::npos) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   in[p].span,
                   std::string{"#line requires a digit sequence — got: "}
                       + std::string{numTx});
            return;
        }
        unsigned long long n = 0;
        for (char const c : numTx) {
            n = n * 10ull + static_cast<unsigned long long>(c - '0');
            if (n > 2147483647ull) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), in[p].span, "#line number out of range");
                return;
            }
        }
        // C23 6.10.4p2 constrains the digit sequence to 1..2147483647 — the range
        // is TWO-sided. `#line 0` was silently accepted before (gcc
        // -pedantic-errors rejects it), which would make `__LINE__` 0.
        if (n == 0) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   in[p].span, "#line number out of range");
            return;
        }
        LineDirectiveRec rec;
        rec.presumedLine = static_cast<std::uint32_t>(n);

        // OPTIONAL "file" operand (6.10.4p3): absent => presumed name UNCHANGED.
        std::size_t const q = skipTrivia(in, p + 1);
        if (q < end && !isNewline(in[q])) {
            // A quoted operand is THREE tokens, exactly as `#include`/`#embed` see
            // it: the OPENER (config kind `quoteIncludeToken`, which consumed only
            // the `"`), the coalesced BODY token whose text is the raw bytes
            // between the quotes, and the CLOSER
            // (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN). Keying on the CONFIG kinds +
            // position keeps this agnostic — never a hard-coded `"` scan of one
            // token's text.
            if (quoteIncludeKind_.valid() && in[q].schemaKind == quoteIncludeKind_
                && q + 1 < end && !isNewline(in[q + 1])) {
                rec.file    = std::string{text(in[q + 1])};
                rec.hasFile = true;
                // Reject trailing junk LOUDLY, mirroring handleEmbed: silently
                // ignoring tokens after the operand would let an unsupported
                // form be half-honoured (`#line 5 "f" <anything>`). The scan
                // starts past the CLOSER, not at `q + 2` — the closing `"` is a
                // real token now and would otherwise BE the reported junk,
                // rejecting every well-formed `#line 100 "f.c"`.
                std::size_t const t = skipTrivia(in, pastBodyAndCloser(in, q + 1));
                if (t < end && !isNewline(in[t])) {
                    emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                           synth_->id(), in[t].span,
                           std::string{"unexpected token after the #line file "
                                       "operand: "} + std::string{text(in[t])});
                    return;
                }
            } else {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), in[q].span,
                       "#line file operand must be a \"quoted\" string");
                return;
            }
        }

        recordPresumedPosition(in, dirTok, end, rec);
    }

    // A run of decimal digits and nothing else. The linemarker RECOGNIZER: the
    // GNU form has no directive WORD to match a configured spelling against, so
    // its shape is what identifies it (`# 1 "f"`, never `# line 1 "f"`).
    [[nodiscard]] static bool isDecimalRun(std::string_view s) {
        return !s.empty()
               && s.find_first_not_of("0123456789") == std::string_view::npos;
    }

    // The declared linemarker flag whose spelling is `digits`, or nullptr.
    // Config lookup, never a hard-coded `1`/`2`/`3`/`4` — an undeclared digit is
    // refused by the caller, which is what both references do.
    [[nodiscard]] PreprocessConfig::LineMarkerFlagDef const*
    lineMarkerFlag(std::string_view digits) const {
        if (!cfg().lineMarker.has_value()) return nullptr;
        for (auto const& f : cfg().lineMarker->flags) {
            if (f.digits == digits) return &f;
        }
        return nullptr;
    }

    // ── D-C-PREPROCESSED-INPUT-REFUSES-GCC-LINEMARKERS ────────────────────────
    //
    // The GNU LINEMARKER: `# N "file"` optionally followed by flag digits, which
    // is what `gcc -E` / `clang -E` write in place of a `#line`. Same facility,
    // same record, same `presumedLine`/`presumedFile` readers — only the spelling
    // differs, which is why the tail below is `recordPresumedPosition` and not a
    // second copy of it.
    //
    // ⚠ THREE PLACES THIS DELIBERATELY DIVERGES FROM `handleLine`, each measured:
    //
    //  (1) LINE ZERO IS LEGAL HERE AND ILLEGAL IN `#line`. C23 6.10.4p2 constrains
    //      the `#line` digit sequence to 1..2147483647, and `handleLine` enforces
    //      that. ✔MEASURED: gcc 13.3.0's OWN `-E` output opens with `# 0 "tu.c"`
    //      and gcc recompiles that output with rc=0, so importing `#line`'s floor
    //      would make DSS refuse the very bytes this row exists to read.
    //  (2) THE FILE OPERAND IS REQUIRED, not optional. ✔MEASURED over 154 gcc and
    //      177 clang linemarkers in one `_GNU_SOURCE` TU: every single one carries
    //      a quoted name. A bare `# 5` is refused rather than guessed at.
    //  (3) THE TAIL IS FLAGS, NOT JUNK. `handleLine` rejects anything after its
    //      file operand; here the tail is a declared vocabulary and is validated
    //      against it.
    void handleLineMarker(std::vector<Token> const& in, std::size_t p,
                          std::size_t end) {
        std::size_t const dirTok = p;
        // The caller reached this arm on `isDecimalRun`, so the range check is
        // the only numeric failure left.
        std::string_view const numTx = text(in[p]);
        unsigned long long     n     = 0;
        for (char const c : numTx) {
            n = n * 10ull + static_cast<unsigned long long>(c - '0');
            if (n > 2147483647ull) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), in[p].span,
                       "linemarker line number out of range");
                return;
            }
        }
        LineDirectiveRec rec;
        rec.presumedLine = static_cast<std::uint32_t>(n);

        std::size_t const q = skipTrivia(in, p + 1);
        if (q >= end || isNewline(in[q])) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   in[p].span,
                   std::string{"a linemarker requires a \"quoted\" file name "
                               "after the line number — got a bare '"}
                       + std::string{numTx} + "'");
            return;
        }
        // A quoted operand is THREE tokens, exactly as `#include`/`#embed`/`#line`
        // see it: the OPENER (config kind `quoteIncludeToken`), the coalesced BODY
        // whose text is the raw bytes between the quotes, and the CLOSER
        // (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN). Keyed on the CONFIG kind, never
        // a hard-coded `"` scan.
        if (!(quoteIncludeKind_.valid() && in[q].schemaKind == quoteIncludeKind_
              && q + 1 < end && !isNewline(in[q + 1]))) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   in[q].span,
                   "a linemarker's file operand must be a \"quoted\" string");
            return;
        }
        rec.file    = std::string{text(in[q + 1])};
        rec.hasFile = true;

        // The FLAG TAIL. Every rule here is one BOTH references enforce —
        // ✔MEASURED: gcc 13.3.0 and clang 19.1.1 each refuse `# 1 "f.c" 9` (an
        // undeclared digit) and each refuse `# 1 "f.c" 1 2` (both members of the
        // enter/return pair). Refusing is therefore matching the references, not
        // being stricter than them.
        std::unordered_set<std::string>                    seenFlags;
        std::unordered_map<std::string, std::string>       claimedGroups;
        std::size_t t = skipTrivia(in, pastBodyAndCloser(in, q + 1));
        while (t < end && !isNewline(in[t])) {
            std::string const flagTx{text(in[t])};
            PreprocessConfig::LineMarkerFlagDef const* const def =
                lineMarkerFlag(flagTx);
            if (def == nullptr) {
                std::string known;
                if (cfg().lineMarker.has_value()) {
                    for (auto const& f : cfg().lineMarker->flags) {
                        if (!known.empty()) known += ", ";
                        known += f.digits + " (" + f.name + ")";
                    }
                }
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), in[t].span,
                       std::string{"unknown linemarker flag '"} + flagTx
                           + "' — this language declares: "
                           + (known.empty() ? std::string{"<none>"} : known));
                return;
            }
            if (!seenFlags.insert(flagTx).second) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                       synth_->id(), in[t].span,
                       std::string{"linemarker flag '"} + flagTx + "' ("
                           + def->name + ") appears more than once");
                return;
            }
            if (!def->exclusiveGroup.empty()) {
                auto const [it, fresh] =
                    claimedGroups.try_emplace(def->exclusiveGroup, flagTx);
                if (!fresh) {
                    emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                           synth_->id(), in[t].span,
                           std::string{"linemarker flags '"} + it->second
                               + "' and '" + flagTx
                               + "' are mutually exclusive (both are '"
                               + def->exclusiveGroup + "')");
                    return;
                }
            }
            t = skipTrivia(in, t + 1);
        }

        // ★ WHAT THE FLAGS DO, DECIDED AND STATED RATHER THAN DROPPED IN SILENCE.
        // The enter/return pair describes the include-STACK transition; DSS's
        // presumed-position model is per ORIGIN BUFFER and a preprocessed TU is
        // one buffer, so both spellings mean the same thing to this record — the
        // following lines come from the named file starting at the given number —
        // and the pair is declared so that `1 2` together can be REFUSED.
        // ✔`system-header` is RECOGNISED and carries no suppression, and that is a
        // UNIFORM policy rather than a hole in this directive: MEASURED at HEAD,
        // `src/` contains no system-header concept at all, so DSS already declines
        // to suppress diagnostics inside an ordinary `#include <...>` header. A TU
        // fed through `gcc -E` therefore reports exactly what DSS would report
        // compiling the same headers directly — which is the property that makes
        // this row's conformance census meaningful in the first place. If DSS ever
        // gains a system-header posture, THIS is the flag that selects it, and the
        // pin in tests/analysis/preprocess/test_preprocessor.cpp is what will go
        // red when it does. `extern-c-linkage` is inert BY CONSTRUCTION, not by
        // omission: a C front end has no second linkage to switch to.
        recordPresumedPosition(in, dirTok, end, rec);
    }

    void handleUndef(std::vector<Token> const& in, std::size_t p,
                     std::size_t end) {
        p = skipTrivia(in, p);
        if (p >= end || isNewline(in[p]) || !isWord(in[p])) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorDirective, synth_->id(),
                   (p < end ? in[p].span : SourceSpan::empty(0)),
                   "#undef requires a macro name");
            return;
        }
        const std::string name{text(in[p])};
        // FC15b (C23 6.10.10.1p2): an `#undef` of a name this implementation
        // predefined (config-seeded lookup, no hard-coded name).
        //
        // ★ D-PP-PREDEFINE-REDEFINITION-PARTITION: the `#undef` half, and it is
        // the half where the two classes separate most cleanly. ✔MEASURED
        // 2026-08-26 on gcc 13.3.0 and clang 18.1.3, probed separately: a bare
        // `#undef __STDC_VERSION__` DIAGNOSES on both, while an `#undef` of an
        // ordinary compiler- or arch-identity row is SILENT on both (16 such
        // names swept, zero diagnostics between them) — the
        // warn-class diagnostic is attached to the NAME, the ordinary one only
        // to a REDEFINITION. An `ordinary` row is not in `predefined_` at all
        // (it lowers to a "<built-in>" `#define`), so its `#undef` reaches the
        // ordinary handler below and simply erases it, silently.
        //
        // ★★ WARN AND FALL THROUGH — see the twin block in `handleDefine` for
        // why this stopped being an error. ✔MEASURED on both references: after
        // a bare `#undef`, `#ifdef __LINE__` / `__STDC__` / `__COUNTER__` are
        // all FALSE, i.e. the directive really removes the name. The `erase`
        // below is what reproduces that; without it `isDefined` would keep
        // answering true from `predefined_` and the `#undef` would be a
        // diagnostic with no effect, which is the worst of both readings.
        if (auto pit = predefined_.find(name); pit != predefined_.end()) {
            if (predefinedNameIsDiagnosedOnChange(
                    pit->second.programRedefinition)) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorPredefinedMacro,
                       synth_->id(), in[p].span,
                       std::string{"'"} + name
                           + "' is a predefined macro of this implementation; "
                             "#undef'ing it removes the implementation's value",
                       DiagnosticSeverity::Warning);
            }
            predefined_.erase(pit);
        }
        // TF-C86 (D-CSUBSET-STDARG-F001A): the `#undef` half. An `#undef
        // __has_include` that SUCCEEDED would silently turn the operator back
        // into an ordinary undefined identifier folding to 0 — the same
        // include-vs-guard disagreement the `#define` arm refuses.
        if (isConditionalInclusionOperator(name, cfg())) {
            emitPP(rep_,
                   DiagnosticCode::P_PreprocessorOperatorNameNotDefinable,
                   synth_->id(), in[p].span,
                   std::string{"'"} + name
                       + "' is a conditional-inclusion operator this "
                         "implementation provides and may not be #undef'd");
            return;
        }
        // FC18a (D-PP-VA-OPT): the `#undef` half of the `__VA_OPT__` name guard.
        // An `#undef __VA_OPT__` that SUCCEEDED would claim to remove something
        // that was never in the macro table, and would read as license to then
        // `#define` it -- the same one-spelling-two-meanings hazard the `#define`
        // arm refuses. ✔MEASURED: cl 19.51 answers C4117 ("reserved, '#undef'
        // ignored"); clang/gcc reject under -pedantic-errors.
        if (!cfg().vaOptName.empty() && name == cfg().vaOptName) {
            emitPP(rep_,
                   DiagnosticCode::P_PreprocessorOperatorNameNotDefinable,
                   synth_->id(), in[p].span,
                   std::string{"'"} + name
                       + "' is a variadic-macro operator this implementation "
                         "provides and may not be #undef'd");
            return;
        }
        table_.erase(name);
    }

    // Index of the next non-trivia / non-newline token from the cursor. A
    // function-like invocation lookahead PEEKS past intervening whitespace AND
    // newlines (`FOO\n(1)` is a valid call: once the directive line itself is
    // stripped, C 6.10.3p10/p11 treat the name and the `(` as adjacent across
    // white space, including line breaks). Returns in.size() if none.
    static std::size_t nextSignificant(std::deque<ExpToken> const& in,
                                       std::size_t from) {
        std::size_t j = from;
        while (j < in.size()
               && (isTrivia(in[j].tok) || isNewline(in[j].tok))) ++j;
        return j;
    }

    // Trim leading + trailing whitespace-trivia (incl. newlines/comments) from
    // an argument token run, in place. Interior trivia is PRESERVED (an
    // argument may legitimately contain spaces, e.g. `a + b`).
    static void trimArgTrivia(std::vector<ExpToken>& arg) {
        std::size_t a = 0, b = arg.size();
        while (a < b && (isTrivia(arg[a].tok) || isNewline(arg[a].tok))) ++a;
        while (b > a
               && (isTrivia(arg[b - 1].tok) || isNewline(arg[b - 1].tok))) --b;
        if (a != 0 || b != arg.size()) {
            arg = std::vector<ExpToken>(
                arg.begin() + static_cast<std::ptrdiff_t>(a),
                arg.begin() + static_cast<std::ptrdiff_t>(b));
        }
    }

    // Collect a function-like macro call argument list. `in[open]` is the
    // invocation `(`. Scans tracking PAREN DEPTH (only the configured open/close
    // paren affect depth): a depth-1 comma SEPARATES arguments; the matching
    // depth-0 close ENDS the list. Each argument preserves its interior tokens
    // (nested parens/commas survive) AND their hide sets; leading/trailing trivia
    // is trimmed. By the comma-separated-groups rule, `(x)` is ONE argument and
    // `()` is ONE EMPTY argument; the special zero-PARAMETER case (`M()` for a
    // 0-arg macro = zero arguments, C 6.10.3p4) is normalized by the CALLER (which
    // knows params.size()). The depth-1 SEPARATOR tokens are also recorded into
    // `separators` (one per top-level comma -> `args.size()-1` entries): a
    // VARIADIC macro re-joins the trailing arguments with the ORIGINAL separator
    // tokens to form `__VA_ARGS__` (preserving the source commas, C 6.10.3p4),
    // rather than synthesizing one. On success returns the arguments, sets `past`
    // to the index JUST PAST the matching close, and copies the CLOSING paren's
    // hide set into `closeHide` (the Prosser function-like rule intersects the
    // macro NAME's hide set with the CLOSE paren's). On EOF before the matching
    // close, emits a fail-loud diagnostic and returns std::nullopt.
    std::optional<std::vector<std::vector<ExpToken>>>
    collectArgs(std::deque<ExpToken> const& in, std::size_t open,
                std::string const& macroName, std::size_t& past,
                std::vector<ExpToken>& separators, HideSet& closeHide) {
        std::vector<std::vector<ExpToken>> args;
        std::vector<ExpToken>              cur;
        int depth = 1;                 // start just inside the opening `(`
        std::size_t j = open + 1;
        for (; j < in.size(); ++j) {
            ExpToken const& t = in[j];
            if (isParenOpen(t.tok)) {
                ++depth;
                cur.push_back(t);
                continue;
            }
            if (isParenClose(t.tok)) {
                --depth;
                if (depth == 0) {
                    // End of list: flush the final (possibly empty) group and
                    // surface the close paren's hide set for the Prosser rule.
                    trimArgTrivia(cur);
                    args.push_back(std::move(cur));
                    past      = j + 1;
                    closeHide = t.hide;
                    return args;
                }
                cur.push_back(t);
                continue;
            }
            if (depth == 1 && isArgSeparator(t.tok)) {
                // Top-level separator: close current argument, start the next.
                // Record the separator token verbatim (the variadic catch-all
                // re-joins trailing args with these ORIGINAL commas).
                trimArgTrivia(cur);
                args.push_back(std::move(cur));
                cur.clear();
                separators.push_back(t);
                continue;
            }
            cur.push_back(t);
        }
        // Ran off the end without the matching close -> unterminated.
        emitPP(rep_, DiagnosticCode::P_PreprocessorMacroArgument, synth_->id(),
               (open < in.size() ? in[open].tok.span : SourceSpan::empty(0)),
               std::string{"unterminated argument list for function-like "
                           "macro: "}
                   + macroName);
        return std::nullopt;
    }

    // Map a replacement token that names a NAMED parameter to its parameter
    // index, or -1. (`def.replacement` carries only significant tokens -- trivia
    // was dropped at #define time -- so a parameter is always a bare `Word`.)
    [[nodiscard]] int paramIndexOf(Token const& r, MacroDef const& def) const {
        if (!isWord(r)) return -1;
        std::string_view rt = text(r);
        for (std::size_t k = 0; k < def.params.size(); ++k) {
            if (def.params[k] == rt) return static_cast<int>(k);
        }
        return -1;
    }
    [[nodiscard]] bool isVaArgsName(Token const& r, MacroDef const& def) const {
        return def.isVariadic && isWord(r) && !cfg().variadicArgsName.empty()
            && text(r) == cfg().variadicArgsName;
    }

    // ── FC18a (D-PP-VA-OPT, C23 6.10.5.1) ────────────────────────────────────
    //
    // The va-opt-replacement INTRODUCER, matched by config TEXT (`vaOptName`).
    // This form takes no `MacroDef`: it answers "is this token the introducer
    // identifier", which is what the DEFINITION-time constraint checks need
    // (they must fire precisely in the macros where the construct is NOT
    // allowed). An empty config spelling means the language declares no va-opt
    // construct, so nothing is ever recognized.
    [[nodiscard]] bool isVaOptWord(Token const& r) const {
        return isWord(r) && !cfg().vaOptName.empty()
            && text(r) == cfg().vaOptName;
    }
    // The SUBSTITUTION-time form: a va-opt is only ever acted on inside a
    // variadic macro, which `handleDefine` has already guaranteed.
    [[nodiscard]] bool isVaOptName(Token const& r, MacroDef const& def) const {
        return def.isVariadic && isVaOptWord(r);
    }

    // Locate the `)` that closes the va-opt whose introducer sits at `introIdx`
    // in `repl`, "determined by skipping intervening pairs of matching left and
    // right parentheses in its pp-tokens" (C23 6.10.5.1p3).
    //
    // Returns the index of the CLOSING paren. `openOut` receives the index of the
    // opening paren. Returns `npos` when the construct is malformed -- either the
    // introducer is not followed by `(`, or the `(` is never closed. The caller
    // decides which diagnostic that is (it has the context to name it), which is
    // why this reports a plain failure and emits nothing itself.
    [[nodiscard]] static constexpr std::size_t vaOptNpos() {
        return static_cast<std::size_t>(-1);
    }

    // "Does this run contain at least one PREPROCESSING TOKEN?" -- the emptiness
    // question C23 6.10.5.1p7 actually asks of a va-opt's controlling
    // substitution ("consists of no preprocessing tokens").
    //
    // ★ WHITESPACE IS NOT A PREPROCESSING TOKEN, AND A PLAIN `.empty()` GETS THIS
    // WRONG. `collectArgs`/`trimArgTrivia` strip only the LEADING and TRAILING
    // trivia of an argument, so an argument whose every macro expanded away can
    // still leave INTERIOR white space behind. ✔MEASURED, this is observable:
    // with `#define EMP`, `F(EMP)` has nothing left and `F(EMP EMP)` is left
    // holding one whitespace token — yet clang-18, clang-19 and gcc-13 all answer
    // `f(0 )` for BOTH, because neither has any preprocessing token left. A
    // `.empty()` test passes the first and fails the second, which is exactly the
    // split this project's own oracle differential caught.
    [[nodiscard]] static bool hasSignificantToken(
        std::vector<ExpToken> const& run) {
        for (ExpToken const& e : run) {
            if (e.placemarker) continue;             // not a real token either
            if (isTrivia(e.tok) || isNewline(e.tok)) continue;
            return true;
        }
        return false;
    }
    [[nodiscard]] std::size_t findVaOptClose(std::vector<Token> const& repl,
                                             std::size_t introIdx,
                                             std::size_t& openOut) const {
        const std::size_t n = repl.size();
        const std::size_t open = introIdx + 1;
        if (open >= n || !isParenOpen(repl[open])) return vaOptNpos();
        openOut = open;
        std::size_t depth = 0;
        for (std::size_t j = open; j < n; ++j) {
            if (isParenOpen(repl[j])) {
                ++depth;
            } else if (isParenClose(repl[j])) {
                if (--depth == 0) return j;
            }
        }
        return vaOptNpos();
    }

    // Build the substituted replacement list for a function-like call (C 6.10.3).
    // A replacement token that names a parameter (or `__VA_ARGS__`) is replaced by
    // that argument's tokens; every other token passes through stamped with `hs`.
    // FC15a adds the `#` (stringize, C 6.10.3.2) and `##` (token-paste, C 6.10.3.3)
    // operators in TWO phases:
    //   PHASE A -- substitution. A normal parameter substitutes its PRE-EXPANDED
    //   argument (`expandedArgs[k]` / `vaArgs`, C 6.10.3.1). A `#` immediately
    //   followed by a parameter is replaced by ONE string-literal product
    //   (`stringizeTokens`, F2) built from that parameter's RAW argument
    //   (`rawArgs[k]` / `rawVaArgs`). A parameter that is an OPERAND of a `##`
    //   (its adjacent significant replacement token is a `##`) substitutes its RAW
    //   argument (C 6.10.3.1: `#`/`##` operands are NOT pre-expanded). `##` tokens
    //   are kept verbatim as MARKERS for phase B.
    //   PHASE B -- paste. Each `##` MARKER is collapsed LEFT-TO-RIGHT: the last
    //   significant token to its left and the first to its right are concatenated
    //   into a single re-tokenized product (`pasteTokens`, F1), then a rescan
    //   continues from that product (so `a##b##c` chains).
    // Fail-loud (each with best-effort recovery): a `#` not followed by a
    // parameter -> P_PreprocessorStringize; a `##` at the start/end of the list,
    // or a paste whose spelling is not a single token -> P_PreprocessorPaste.
    //
    // HIDE-SET stamping (Prosser, C 6.10.3.4): EVERY token of the substituted
    // result carries `hs` = (hideset(name) ∩ hideset(close-paren)) ∪ {M}. A
    // replacement-origin token (plain, or a `#`/`##` product) is stamped with
    // exactly `hs`; an argument token already carries its own (accreted) hide set,
    // UNIONED with `hs`.
    std::vector<ExpToken> substitute(
        MacroDef const& def,
        std::vector<std::vector<ExpToken>> const& expandedArgs,
        std::vector<ExpToken> const& vaArgs,
        std::vector<std::vector<ExpToken>> const& rawArgs,
        std::vector<ExpToken> const& rawVaArgs,
        HideSet const& hs, ByteOffset invOffset) {
        std::vector<ExpToken> items;
        substituteRange(def, 0, def.replacement.size(), expandedArgs, vaArgs,
                        rawArgs, rawVaArgs, hs, invOffset, items);
        // ── PHASE B: collapse every `##` marker LEFT-TO-RIGHT. ──
        return collapsePastes(std::move(items), hs, invOffset,
                              /*sweepPlacemarkers=*/true);
    }

    // PHASE A over the HALF-OPEN replacement-list range `[begin, end)`, appending
    // to `items`. The whole-list call (`begin=0, end=size()`) is the ordinary
    // macro path and behaves exactly as before this function was made
    // range-scoped; the RESTRICTED call is FC18a's va-opt-replacement content
    // (C23 6.10.5.1p7: the content is expanded "as the replacement list of the
    // current function-like macro"), which is why one walk serves both.
    //
    // ★ ADJACENCY IS RANGE-LOCAL, AND THAT IS THE SEMANTICS, NOT AN ARTIFACT.
    // The `#`/`##`-operand tests below look at `i > begin` / `i + 1 < end`, not at
    // the whole list. In `__VA_OPT__(a X) ## b` the `##` binds to the va-opt's
    // RESULT, not to `X`, so `X` inside the content is not a paste operand; in
    // `__VA_OPT__(a X ## X) ## b` the interior `##` IS in range and does bind.
    // ✔MEASURED: the second is the standard's own H4, and clang-18/clang-19/
    // gcc-13 all answer `a b` for `H4(, 1)` -- which only comes out right when
    // the interior paste is evaluated and the exterior one is not.
    //
    // Each appended token's `spacedBefore` is maintained UNCONDITIONALLY -- this walk
    // is writer (2) of that field for replacement-list tokens, and the propagator for
    // argument tokens. There is no opt-in flag: a bit that is only sometimes true is
    // a bit no reader can trust, and the one that used to gate this (`trackSpacing`,
    // set solely for the `#__VA_OPT__(...)` content walk) is exactly why `#param`
    // could not share the va-opt spelling builder and kept its own broken one.
    void substituteRange(
        MacroDef const& def, std::size_t begin, std::size_t end,
        std::vector<std::vector<ExpToken>> const& expandedArgs,
        std::vector<ExpToken> const& vaArgs,
        std::vector<std::vector<ExpToken>> const& rawArgs,
        std::vector<ExpToken> const& rawVaArgs,
        HideSet const& hs, ByteOffset invOffset, std::vector<ExpToken>& items) {
        // FC15b: a REPLACEMENT-origin token (a plain replacement token, a `##`
        // marker/product, a stringize product) inherits the INVOCATION offset
        // `invOffset` (so a `__LINE__` in the replacement resolves to the
        // invocation line). An ARGUMENT token keeps its OWN `invOffset` (it came
        // from the call site -- its real position).
        //
        // ★ COPY-THEN-ADJUST, never field-by-field reconstruction
        // (D-PP-SPACING-BIT-NOT-ACTUALLY-CARRIED). An argument token
        // arrives carrying `spacedBefore` (its adjacency, settled where it was
        // produced); rebuilding the ExpToken from three fields silently RESET that
        // bit to `false`, so every argument token after the first stringized as
        // though it had been jammed against its predecessor. The hide set is the only
        // thing substitution changes here.
        auto stampArg = [&](std::vector<ExpToken> const& a,
                            std::vector<ExpToken>& outTokens) {
            for (ExpToken const& e : a) {
                ExpToken stamped = e;
                stamped.hide     = hideUnionAll(e.hide, hs);
                outTokens.push_back(std::move(stamped));
            }
        };
        // FC15 paste residuals (D-PP-PASTE-PLACEMARKER, C 6.10.3.3p2): stamp a
        // `##`-OPERAND argument, but when that argument is EMPTY emit a single
        // PLACEMARKER instead of nothing. This is the crux that lets Phase B tell
        // an empty-argument operand (valid -> `x ## <empty>` = `x`) apart from a
        // GENUINE dangling `##` (no operand token at all in the replacement list,
        // which still trips the boundary check in `collapsePastes`). Used ONLY for
        // `##`-operand positions; a non-operand empty arg still vanishes.
        auto stampArgOrPM = [&](std::vector<ExpToken> const& a,
                                std::vector<ExpToken>& outTokens) {
            if (a.empty()) {
                ExpToken pm{};
                pm.hide = hs;
                pm.invOffset = invOffset;
                pm.placemarker = true;
                outTokens.push_back(pm);
            } else {
                stampArg(a, outTokens);
            }
        };
        // The RAW token run for a `#`/`##` operand at replacement index `i`
        // (a named parameter or `__VA_ARGS__`). Returns nullptr if `i` is not a
        // parameter position.
        auto rawArgAt =
            [&](std::size_t i) -> std::vector<ExpToken> const* {
            Token const& r = def.replacement[i];
            if (isVaArgsName(r, def)) return &rawVaArgs;
            int const pi = paramIndexOf(r, def);
            if (pi >= 0) return &rawArgs[static_cast<std::size_t>(pi)];
            return nullptr;
        };

        // The run an arm just appended (everything from index `mark` on) begins where
        // the REPLACEMENT token at `i` sat, so its first token takes the
        // replacement-level answer `sp`. That is the ONLY bit this function owns.
        //
        // ★★ TOKENS 2..N ARE NOT THIS FUNCTION'S BUSINESS, AND PRETENDING OTHERWISE
        // WAS A DEFECT (D-PP-SPACING-BIT-NOT-ACTUALLY-CARRIED). This used to
        // recompute them as
        // `items[k].span.start() != items[k-1].span.end()`, on the premise that a run
        // came from ONE argument's contiguous call-site text. That premise is false
        // the moment the argument itself arrived through an expansion: its tokens then
        // come from the `#define` line, the call site and `productText_` interleaved,
        // and the comparison is reading unrelated byte positions. ✔MEASURED at the
        // one place it was already reachable (`#__VA_OPT__(X)` with X = `CAT2(z)`,
        // `#define CAT2(a) a+b`): DSS said "z +b" where all three oracles say "z+b".
        // Those tokens carry their OWN correct bit (`stampArg` propagates it, the
        // recursive va-opt walk sets it) -- so leave them alone.
        auto applySpacing = [&](std::size_t mark, bool sp) {
            if (items.size() <= mark) return;
            items[mark].spacedBefore = sp;
        };

        // ── PHASE A: substitution (keeping `##` markers verbatim). ──
        const std::size_t n = end;
        for (std::size_t i = begin; i < n; ++i) {
            Token const& r = def.replacement[i];
            // White space preceded `r` iff it does not start exactly where its
            // predecessor ended. (`def.replacement` holds only SIGNIFICANT tokens
            // -- trivia was dropped at `#define` time -- so the gap is the only
            // remaining evidence, and it is exact.)
            const bool spacedHere =
                i > begin
                && def.replacement[i - 1].span.end() != r.span.start();
            const std::size_t mark = items.size();
            if (isStringize(r)) {
                // `#` operand is the NEXT significant token, which MUST be a
                // parameter (or `__VA_ARGS__`), or -- FC18a, C23 6.10.5.1p4,
                // "a va-opt-replacement is treated as if it were a parameter" --
                // a whole `__VA_OPT__( ... )`.
                if (i + 1 < n) {
                    if (isVaOptName(def.replacement[i + 1], def)) {
                        std::size_t open = 0;
                        const std::size_t close =
                            findVaOptClose(def.replacement, i + 1, open);
                        // `handleDefine` rejects a malformed va-opt, so a
                        // well-formed table entry always resolves. The guard is
                        // defense-in-depth: a `npos` here would otherwise index
                        // out of bounds.
                        if (close != vaOptNpos() && close < n) {
                            stringizeVaOpt(def, open + 1, close, expandedArgs,
                                           vaArgs, rawArgs, rawVaArgs, hs,
                                           invOffset, items);
                            applySpacing(mark, spacedHere);
                            i = close;   // consume `__VA_OPT__ ( ... )`
                            continue;
                        }
                    }
                    if (auto const* raw = rawArgAt(i + 1)) {
                        // THE SAME builder the `#__VA_OPT__(...)` arm above uses --
                        // one mechanism for both `#` operands, so they cannot drift.
                        stringizeTokens(*raw, hs, invOffset, items);
                        applySpacing(mark, spacedHere);
                        ++i;   // consume the parameter operand
                        continue;
                    }
                }
                emitPP(rep_, DiagnosticCode::P_PreprocessorStringize, synth_->id(),
                       r.span,
                       "'#' in a macro replacement must be followed by a "
                       "parameter");
                items.push_back(ExpToken{r, hs, invOffset});  // recovery: `#` verbatim
                applySpacing(mark, spacedHere);
                continue;
            }
            if (isPaste(r)) {
                items.push_back(ExpToken{r, hs, invOffset});  // marker for phase B
                applySpacing(mark, spacedHere);
                continue;
            }
            // ── FC18a (D-PP-VA-OPT, C23 6.10.5.1p7): a va-opt-replacement. ──
            //
            // The construct spans `__VA_OPT__ ( content )` -- several replacement
            // tokens -- so it is recognized here and CONSUMED whole.
            //
            // ★★ THE EMPTINESS PREDICATE IS `vaArgs`, NOT `rawVaArgs`, AND THE
            // DIFFERENCE IS OBSERVABLE. 6.10.5.1p7 keys the choice on "a
            // (hypothetical) substitution of __VA_ARGS__ as neither an operand of
            // # nor ##" -- i.e. the MACRO-EXPANDED variable arguments. The
            // standard's own EXAMPLE 2 is the witness: with `#define EMP` and
            // `#define F(...) f(0 __VA_OPT__(,) __VA_ARGS__)`, `F(EMP)` is
            // "replaced by f(0)" -- one RAW argument token is present, yet its
            // SUBSTITUTION is empty, so the comma goes. ✔MEASURED on clang-18,
            // clang-19 and gcc-13, which also answer `f(0)` for `F(EMP EMP)` (two
            // raw tokens, still an empty substitution). The GNU comma-elision arm
            // above deliberately keeps testing `rawVaArgs` -- ✔MEASURED, those
            // same three compilers answer `g(1 , )` for the GNU spelling of the
            // identical call. Two constructs, two predicates, both correct.
            if (isVaOptName(r, def)) {
                std::size_t open = 0;
                const std::size_t close = findVaOptClose(def.replacement, i, open);
                if (close == vaOptNpos() || close >= n) {
                    // `handleDefine` rejects this at DEFINITION time, so reaching
                    // here means the table holds a malformed entry. Fail loud
                    // rather than index past the range.
                    emitPP(rep_, DiagnosticCode::P_PreprocessorDirective,
                           synth_->id(), r.span,
                           std::string{"'"} + cfg().vaOptName
                               + "' is missing its closing parenthesis");
                    continue;
                }
                if (!hasSignificantToken(vaArgs)) {
                    // Empty substitution -> a single PLACEMARKER (6.10.5.1p7).
                    // A placemarker, not "nothing": it is what lets an adjacent
                    // `##` still find an operand, so `z ## __VA_OPT__(w)` with no
                    // variable arguments collapses to `z` instead of tripping the
                    // dangling-`##` constraint check. ✔MEASURED `z` on all three.
                    ExpToken pm{};
                    pm.hide      = hs;
                    pm.invOffset = invOffset;
                    pm.placemarker = true;
                    items.push_back(pm);
                } else {
                    // Non-empty substitution -> the content, expanded as this
                    // macro's replacement list. Recursion reuses THIS walk, so
                    // parameters, `__VA_ARGS__`, `#` and `##` all behave inside a
                    // va-opt exactly as they do outside it -- no second engine to
                    // keep in agreement.
                    const std::size_t before = items.size();
                    substituteRange(def, open + 1, close, expandedArgs, vaArgs,
                                    rawArgs, rawVaArgs, hs, invOffset, items);
                    // Significance, not size: a parameter inside the content
                    // whose argument expanded away can leave interior white space
                    // behind, and white space is not a preprocessing token. The
                    // same distinction `hasSignificantToken` exists for.
                    bool appendedSomething = false;
                    for (std::size_t k = before; k < items.size(); ++k) {
                        if (items[k].placemarker) continue;
                        if (isTrivia(items[k].tok) || isNewline(items[k].tok)) {
                            continue;
                        }
                        appendedSomething = true;
                        break;
                    }
                    if (!appendedSomething) {
                        // The content substituted to NOTHING (it was empty, or it
                        // was a parameter with an empty argument). Same reasoning
                        // as the empty-substitution arm: emit a placemarker so an
                        // adjacent `##` still has an operand. ✔MEASURED: for
                        // `#define PZ(...) z ## __VA_OPT__()`, `PZ(1)` is `z` on
                        // all three oracles -- not a dangling-`##` error.
                        //
                        // REPLACE, don't append: any white space the content did
                        // leave behind must go, or an adjacent `##` would bind to
                        // a whitespace token instead of to the placemarker.
                        items.resize(before);
                        ExpToken pm{};
                        pm.hide      = hs;
                        pm.invOffset = invOffset;
                        pm.placemarker = true;
                        items.push_back(pm);
                    }
                }
                // The run's FIRST token takes the va-opt's own replacement-level
                // spacing; the recursive call already settled the interior bits from
                // the content's own layout. (This arm used to open-code the
                // assignment precisely BECAUSE `applySpacing` would have clobbered
                // those interior bits with span arithmetic. Now that it does not,
                // the two are the same operation and share the one owner.)
                applySpacing(mark, spacedHere);
                i = close;   // consume through the closing `)`
                continue;
            }
            if (isVaArgsName(r, def)) {
                // RAW iff this `__VA_ARGS__` is a `##` operand (adjacent `##`).
                bool const pasteOperand =
                    (i > begin && isPaste(def.replacement[i - 1]))
                    || (i + 1 < n && isPaste(def.replacement[i + 1]));
                // FC15 paste residuals (D-PP-VARIADIC-GNU-COMMA-ELISION): the GNU
                // `sep ## __VA_ARGS__` idiom, CONFIG-gated by `variadicCommaElision`
                // (the separator matched by the config-declared arg-separator KIND,
                // `__VA_ARGS__` by the config `variadicArgsName` -- never a hardcoded
                // `,` byte or name). It fires only when the `##` immediately PRECEDES
                // this `__VA_ARGS__` (left-paste) AND the token before that just-pushed
                // `##` marker in `items` is the separator. EMPTY __VA_ARGS__: drop BOTH
                // the separator and the `##` (the comma vanishes -> `f(fmt)`). NON-empty:
                // drop only the `##` (no paste) and emit the PRE-EXPANDED args after the
                // kept separator (-> `f(fmt, a, b)`). Anything else (flag off, or
                // `p ## __VA_ARGS__` where the left neighbor is a value not a separator)
                // falls through to the standard path below.
                if (cfg().variadicCommaElision
                    && i > begin && isPaste(def.replacement[i - 1])
                    && items.size() >= 2
                    && !items.back().placemarker
                    && isPaste(items.back().tok)
                    && isArgSeparator(items[items.size() - 2].tok)) {
                    if (rawVaArgs.empty()) {
                        items.pop_back();   // drop the `##` marker
                        items.pop_back();   // drop the preceding separator
                    } else {
                        items.pop_back();          // drop only the `##` (no paste)
                        stampArg(vaArgs, items);   // pre-expanded __VA_ARGS__
                    }
                    // The elision arm POPS items, so `mark` may now be past the
                    // end; `applySpacing` is a no-op in that case, which is right
                    // -- nothing new was appended to attribute spacing to.
                    applySpacing(mark, spacedHere);
                    continue;
                }
                // Standard path. A `##`-operand EMPTY `__VA_ARGS__` becomes a
                // PLACEMARKER (so `x ## __VA_ARGS__` with empty args -> `x`, and the
                // flag-off `sep ## __VA_ARGS__` empty case -> the standard
                // `sep ## <pm>` = `sep`); otherwise the raw run for a paste operand,
                // else the pre-expanded run. (MUST-FIX-1: paste-operand fall-through
                // uses stampArgOrPM, not stampArg, so an empty operand is a placemarker.)
                if (pasteOperand) {
                    stampArgOrPM(rawVaArgs, items);
                } else {
                    stampArg(vaArgs, items);
                }
                applySpacing(mark, spacedHere);
                continue;
            }
            int const pi = paramIndexOf(r, def);
            if (pi >= 0) {
                bool const pasteOperand =
                    (i > begin && isPaste(def.replacement[i - 1]))
                    || (i + 1 < n && isPaste(def.replacement[i + 1]));
                // FC15 paste residuals: a `##`-operand parameter with an EMPTY
                // argument becomes a PLACEMARKER (C 6.10.3.3p2) via stampArgOrPM;
                // a non-operand parameter keeps the byte-identical pre-expanded path.
                if (pasteOperand) {
                    stampArgOrPM(rawArgs[static_cast<std::size_t>(pi)], items);
                } else {
                    stampArg(expandedArgs[static_cast<std::size_t>(pi)], items);
                }
                applySpacing(mark, spacedHere);
                continue;
            }
            // A plain replacement token gets EXACTLY hs (no prior hide set) and
            // the INVOCATION offset (FC15b: a `__LINE__` in a function-like
            // replacement resolves to the invocation line).
            items.push_back(ExpToken{r, hs, invOffset});
            applySpacing(mark, spacedHere);
        }
    }

    // Phase B of `substitute`: walk `items`, and at each `##` MARKER concatenate
    // the token immediately before it with the one immediately after into a
    // single re-tokenized product (F1), splicing `[i-1, i, i+1)` -> product and
    // RESCANNING from there (so `a##b##c` collapses left-to-right to one token).
    // A `##` at the very start or end of the list (no operand on one side) is a
    // constraint violation (C 6.10.3.3p1) -> P_PreprocessorPaste, recovery: drop
    // the dangling `##` and keep the lone operand. A product that is not exactly
    // one token (F1) -> P_PreprocessorPaste, recovery: emit both operands verbatim.
    //
    // FC18a: `sweepPlacemarkers` is false ONLY for the `#__VA_OPT__(...)` content
    // collapse. C23 6.10.5.1p7 defines a va-opt's argument as the expansion of its
    // content "BEFORE removal of placemarker tokens", and 6.10.5.2p3 then defines
    // the stringizing argument as that sequence WITH placemarkers removed -- two
    // steps that only compose correctly if the collapse can be asked not to sweep.
    // The ordinary path passes true and is unchanged.
    std::vector<ExpToken> collapsePastes(std::vector<ExpToken> items,
                                         HideSet const& hs,
                                         ByteOffset invOffset,
                                         bool sweepPlacemarkers) {
        std::size_t i = 0;
        while (i < items.size()) {
            // [[D-PP-PASTE-PRODUCT-IS-RE-READ-AS-THE-PASTE-OPERATOR]]: a token
            // this pass MINTED is never an operator, however it is spelled.
            if (!isPaste(items[i].tok) || items[i].pasteProduct) { ++i; continue; }
            SourceSpan const opSpan = items[i].tok.span;
            const bool hasLeft  = (i > 0);
            const bool hasRight = (i + 1 < items.size());
            if (!hasLeft || !hasRight) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorPaste, synth_->id(),
                       opSpan,
                       std::string{"'##' must not appear at the "}
                           + (!hasLeft ? "start" : "end")
                           + " of a macro replacement list");
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                // Resume at the operand that now occupies `i` (or end).
                continue;
            }
            // FC15 paste residuals (D-PP-PASTE-PLACEMARKER, C 6.10.3.3p2): when an
            // operand is a PLACEMARKER (an empty `##`-operand argument), the paste
            // yields the OTHER operand (`pm ## X` -> X, `X ## pm` -> X) and
            // `pm ## pm` -> a placemarker. This runs BEFORE the spelling-concat path
            // so a placemarker is never re-tokenized. The surviving operand is
            // re-stamped with `hs` (the product hide set) by UNION -- mirroring how a
            // real paste product is stamped, never DROPPING the operand's accreted
            // hide set (a dropped name would break Prosser recursion-freezing).
            const bool leftPM  = items[i - 1].placemarker;
            const bool rightPM = items[i + 1].placemarker;
            if (leftPM || rightPM) {
                const std::size_t lo = i - 1;
                ExpToken keep{};
                if (leftPM && rightPM) {
                    keep.hide = hs;
                    keep.invOffset = invOffset;
                    keep.placemarker = true;            // pm ## pm -> pm
                } else if (leftPM) {
                    keep = items[i + 1];                // pm ## X -> X
                    keep.hide = hideUnionAll(keep.hide, hs);
                } else {
                    keep = items[i - 1];                // X ## pm -> X
                    keep.hide = hideUnionAll(keep.hide, hs);
                }
                // Whatever survives now occupies the LEFT operand's position, so it
                // takes that position's leading spacing (see the concat arm below).
                keep.spacedBefore = items[i - 1].spacedBefore;
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(lo),
                            items.begin() + static_cast<std::ptrdiff_t>(i + 2));
                items.insert(items.begin() + static_cast<std::ptrdiff_t>(lo), keep);
                i = lo;   // rescan from the kept operand (chains `a ## pm ## c`)
                continue;
            }
            // Concatenate the spellings of the two operands.
            std::string spelling{text(items[i - 1].tok)};
            spelling += text(items[i + 1].tok);
            auto product = pasteTokens(spelling, opSpan);
            if (!product) {
                // F1 failure (zero or >1 tokens) already reported; recover by
                // dropping the `##` and leaving BOTH operands verbatim. Advance
                // past the left operand so we don't re-paste it.
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                ++i;   // step past the (now adjacent) left operand
                continue;
            }
            // Replace [i-1, i, i+1) with the single product token (hide set hs --
            // a fresh replacement-origin token), then rescan from i-1 so a
            // chained `##` to its right pastes against this product.
            //
            // The product stands where the LEFT operand stood, so it inherits the LEFT
            // operand's `spacedBefore`: the paste consumed the boundary BETWEEN the
            // operands, not the one before them. ✔MEASURED (all three oracles) with
            // `#define P(a,b) a##b`: a two-level stringize of `x P(1,2)` is "x 12" and
            // of `x+P(1,2)` is "x+12" -- the product carries the spacing that preceded
            // the invocation, and the operands' own gap has vanished with the `##`.
            const std::size_t lo = i - 1;
            ExpToken pasted{*product, hs, invOffset};
            // [[D-PP-PASTE-PRODUCT-IS-RE-READ-AS-THE-PASTE-OPERATOR]]
            pasted.pasteProduct = true;
            pasted.spacedBefore = items[lo].spacedBefore;
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(lo),
                        items.begin() + static_cast<std::ptrdiff_t>(i + 2));
            items.insert(items.begin() + static_cast<std::ptrdiff_t>(lo),
                         std::move(pasted));
            i = lo;   // rescan from the product
        }
        // FC15 paste residuals (MUST-FIX-2): drop any PLACEMARKER that survived
        // collapse (e.g. `J(,)` -> a lone placemarker, or a placemarker operand that
        // never met a `##`). Every `##` marker is consumed within this single call,
        // so a surviving placemarker is dead -- removing it HERE guarantees a
        // placemarker never re-enters `expand`'s rescan (the `run()`/`expandTokens()`
        // drop is a defensive backstop only).
        if (sweepPlacemarkers) {
            items.erase(
                std::remove_if(items.begin(), items.end(),
                               [](ExpToken const& e) { return e.placemarker; }),
                items.end());
        }
        return items;
    }

    // FC18a (C23 6.10.5.2 applied to a va-opt operand): STRINGIZE the va-opt whose
    // content occupies `[contentBegin, contentEnd)` of `def.replacement`, appending
    // the string-literal product to `out`.
    //
    // The sequence being stringized is NOT the raw source text of the content: per
    // 6.10.5.1p7 the content is first substituted and `##`-collapsed (placemarkers
    // surviving), and 6.10.5.2p2 then stringizes THAT with placemarkers removed.
    // ✔MEASURED, this is exactly what the standard's H3 needs:
    // `#define H3(X, ...) #__VA_OPT__(X##X X##X)` with `H3(, 0)` is `""` on
    // clang-18/clang-19/gcc-13 -- the two pastes must actually run and yield
    // placemarkers, because spelling the content literally would give "####".
    void stringizeVaOpt(
        MacroDef const& def, std::size_t contentBegin, std::size_t contentEnd,
        std::vector<std::vector<ExpToken>> const& expandedArgs,
        std::vector<ExpToken> const& vaArgs,
        std::vector<std::vector<ExpToken>> const& rawArgs,
        std::vector<ExpToken> const& rawVaArgs,
        HideSet const& hs, ByteOffset invOffset, std::vector<ExpToken>& out) {
        std::vector<ExpToken> content;
        // An EMPTY variable-argument substitution makes the whole va-opt a single
        // placemarker (6.10.5.1p7), which 6.10.5.2p3 then removes -- leaving the
        // empty stringizing argument, whose literal is `""` (6.10.5.2p4).
        // ✔MEASURED `""` for `#define SZ(...) #__VA_OPT__(a)` called `SZ()`.
        if (hasSignificantToken(vaArgs)) {
            substituteRange(def, contentBegin, contentEnd, expandedArgs, vaArgs,
                            rawArgs, rawVaArgs, hs, invOffset, content);
            content = collapsePastes(std::move(content), hs, invOffset,
                                     /*sweepPlacemarkers=*/false);
            content.erase(
                std::remove_if(content.begin(), content.end(),
                               [](ExpToken const& e) { return e.placemarker; }),
                content.end());
        }
        stringizeTokens(content, hs, invOffset, out);
    }

    // ── THE stringize spelling builder (C 6.10.3.2 / C23 6.10.5.2). ONE OWNER. ──
    //
    // Serves BOTH `#param` (a raw argument run) and `#__VA_OPT__(...)` (a substituted
    // content run). There is deliberately no second mechanism: the two used to differ
    // -- `#param` sliced a contiguous byte range -- and the difference WAS
    // D-PP-STRINGIZE-EXPANDED-ARG-SLICES-WRONG-BYTES.
    //
    // ★★ A TOKEN SEQUENCE, NEVER A SOURCE SLICE. A stringizing argument's tokens
    // occupy no single contiguous run of any buffer as soon as one of them came from
    // an expansion -- `XSTR(PLAIN(1,2))` hands `STR` a run whose `g`/`(`/`,`/`)` are
    // from PLAIN's `#define` line and whose `1`/`2` are from the call site. Slicing
    // `front().span.start() .. back().span.end()` across that shipped the define
    // line, a comment, or the remainder of the file, silently.
    //
    // Spelling, per 6.10.5.2p2-p4:
    //  * each preprocessing token's OWN spelling, VERBATIM (see below);
    //  * exactly one space where white space separated it from its predecessor and
    //    NONE where none did -- read from `spacedBefore`, never from spans;
    //  * leading/trailing white space of the argument deleted, which falls out of
    //    skipping trivia and never spacing the first emitted token;
    //  * `\` inserted before each `"` and `\` (`appendEscapedSpelling`).
    //
    // ★ WHITE SPACE INSIDE A TOKEN'S SPELLING SURVIVES VERBATIM
    // (D-PP-STRINGIZE-COLLAPSES-WHITESPACE-INSIDE-A-TOKEN). 6.10.5.2p2 collapses
    // white space BETWEEN the argument's preprocessing tokens; two spaces inside a
    // string or character literal are part of THAT TOKEN'S spelling and are not
    // between anything. ✔MEASURED, clang-18/clang-19/gcc-13/cl 19.51 unanimous:
    // `#define S(x) #x` gives `S("a  b")` -> "\"a  b\"" (both spaces), an interior TAB
    // verbatim, `S('a  b')` -> "'a  b'", and -- the case that pins both rules at once
    // -- `S(f("a  b" ,   "c  d"))` -> "f(\"a  b\" , \"c  d\")", collapsing between
    // tokens while preserving inside them. That is only expressible per-token, which
    // is a second reason the slice had to go: a byte-range walk cannot tell which of
    // its spaces are inside a token.
    //
    // TRIVIA AND PLACEMARKERS ARE SKIPPED, NOT SPELLED. A raw argument run still
    // carries its own white-space tokens (`collectArgs` keeps them) and a va-opt
    // content run can hold placemarkers; neither is a preprocessing token, and their
    // adjacency contribution is already recorded in the next real token's bit.
    //
    // The product is appended to `productText_` and RE-TOKENIZED via
    // `materializeSignificant`, so it reaches the parser as a real opener + body +
    // closer run rather than one fabricated token (a single token would not satisfy
    // the grammar's `stringLiteralExpr`, which since
    // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN names all three slots). Each product token
    // is stamped with `hs`, and with `invOffset` because a `#` product is
    // replacement-origin (FC15b).
    void stringizeTokens(std::vector<ExpToken> const& seq, HideSet const& hs,
                         ByteOffset invOffset, std::vector<ExpToken>& out) {
        std::string inner = "\"";
        bool emittedAny = false;
        for (ExpToken const& e : seq) {
            if (e.placemarker) continue;
            if (isTrivia(e.tok) || isNewline(e.tok)) continue;
            if (emittedAny && e.spacedBefore) {
                inner.push_back(static_cast<char>(0x20));
            }
            appendEscapedSpelling(text(e.tok), inner);
            emittedAny = true;
        }
        inner.push_back('"');
        for (Token const& t : materializeSignificant(inner)) {
            out.push_back(ExpToken{t, hs, invOffset});
        }
    }

    // Append ONE preprocessing token's spelling `src` to `out`, escaping it for life
    // inside a string literal: a `\` before each `"` and `\` (C 6.10.3.2p2). In valid
    // C those two characters occur only inside a string/character literal, so
    // escaping every occurrence is exact.
    //
    // ★ IT DOES NOT TOUCH WHITE SPACE, AND MUST NOT. It used to also collapse every
    // run of white space to one space and drop leading/trailing space -- correct for
    // the whole-argument SOURCE SLICE it was written for, and wrong now that it is
    // handed one token at a time: the two spaces in `S("a  b")` are INSIDE the token
    // and must survive verbatim (✔MEASURED on all four oracles). Between-token
    // spacing is `stringizeTokens`' job, from `spacedBefore`; leading/trailing
    // deletion falls out of skipping trivia there. Two owners of "where do spaces go"
    // is what let the collapse eat bytes it did not own.
    static void appendEscapedSpelling(std::string_view src, std::string& out) {
        for (char const c : src) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
    }

    // FC15b (predefined macros; C 6.10.8.1): compute the once-per-TU translation
    // DATE / TIME spellings. The SPELLINGS themselves live in the exported
    // `translationTimestamp()` (bottom of this file) — the ONE owner, shared with
    // `--dump-predefined-macros`, which has to report the very values this TU will
    // see and would otherwise have re-derived the C-mandated formats itself.
    //
    // `needDate`/`needTime` still gate the STORE, so a TU that predefines neither
    // reads no clock at all. Storing only what was asked for keeps the observable
    // state identical to the pre-extraction shape.
    void computeDateTime(bool needDate, bool needTime) {
        if (!needDate && !needTime) return;
        TranslationTimestamp const ts = translationTimestamp();
        if (needDate) dateString_ = ts.date;
        if (needTime) timeString_ = ts.time;
    }

    // FC15b (predefined macros; C 6.10.8.1): MATERIALIZE the replacement token(s)
    // of a predefined macro at an invocation whose anchor offset is `invOffset`.
    // The spelling is built then routed through `materializeSignificant` (the
    // FC15a A2 mechanism) so the value reaches the REAL parser via a real span in
    // the synth buffer -- a Number for `__LINE__`/Constant, a StringStart +
    // StringLiteral pair for `__FILE__`/`__DATE__`/`__TIME__` (exactly as a
    // stringize product). Dispatches ONLY on `def.kind`, never the name.
    // ── TF-C59 `#line` presumed-position map (C23 6.10.4 / D-CPP-LINE-DIRECTIVE) ──
    // One record per `#line` DIRECTIVE, keyed by the ORIGIN buffer it appeared in
    // (a header and its includer each keep their own numbering) and the PHYSICAL
    // line the directive itself sat on. `#line N` renumbers the line FOLLOWING the
    // directive to N, hence the `-1` in the arithmetic below.
    //
    // Stored per-origin and appended in source order, so the lookup is "the last
    // directive at or before this physical line". Directives are encountered in
    // increasing line order within a buffer, so the vector is sorted by
    // construction — no sort, and a reverse scan finds the active record.
    struct LineDirectiveRec {
        std::uint32_t physLine     = 0;      // line the `#line` itself is on
        std::uint32_t presumedLine = 0;      // N — the number given to physLine+1
        std::string   file;                  // presumed name (empty => inherit)
        bool          hasFile      = false;  // operand present (6.10.4p3)
    };
    std::unordered_map<void const*, std::vector<LineDirectiveRec>> lineDirs_;

    // D-CSUBSET-COUNTER-MACRO-NOT-EXPANDED: the `counter`-kind expansion count for
    // THIS translation unit. Owned by the expander, so its lifetime IS the TU's —
    // see the long note at `PredefinedMacroKind::Counter` in
    // `materializePredefined` for why that is the reset mechanism.
    std::uint32_t counter_ = 0;

    // The SHARED tail of every presumed-position directive: key the record by the
    // ORIGIN buffer the directive physically sits in (so a header and its includer
    // keep independent numbering) and stamp the physical line it ends on.
    //
    // ★ EXTRACTED SO THE `#line` AND LINEMARKER SPELLINGS CANNOT DRIFT
    // (D-C-PREPROCESSED-INPUT-REFUSES-GCC-LINEMARKERS). They are ONE facility with
    // two surfaces; a second copy of this arithmetic is exactly the kind of
    // duplicate that stays correct until one of the two is fixed. It sits HERE,
    // below `LineDirectiveRec`, because the record is a PARAMETER type — a member
    // function BODY may name a nested type declared later, a parameter list may
    // not.
    //
    // Resolve from the LAST token of the directive line, not the first: a
    // directive may span several PHYSICAL lines (a `\` continuation, or a block
    // comment across lines), and the renumbering applies to the line after the
    // directive ENDS. Keying on the first token made every such directive silently
    // off-by-one per extra line. `end` is one past the terminating newline, so
    // `end-1` is that newline (or the last real token at EOF with no trailing
    // newline) — identical for the single-line case.
    void recordPresumedPosition(std::vector<Token> const& in, std::size_t dirTok,
                                std::size_t end, LineDirectiveRec rec) {
        if (lineMap_ == nullptr || lineMap_->empty()) return;
        std::size_t const spanTok =
            (end > 0 && end - 1 < in.size() && end - 1 >= dirTok) ? end - 1
                                                                  : dirTok;
        LineMap::Resolved const r = lineMap_->resolve(in[spanTok].span.start());
        if (r.origin == nullptr) return;
        rec.physLine = r.origin->lineCol(r.offset).line;
        lineDirs_[static_cast<void const*>(r.origin)].push_back(std::move(rec));
    }

    // The active record for (origin, physLine), or nullptr when no `#line`
    // precedes that line in that buffer.
    [[nodiscard]] LineDirectiveRec const* activeLineDir(
        void const* origin, std::uint32_t physLine) const {
        auto const it = lineDirs_.find(origin);
        if (it == lineDirs_.end()) return nullptr;
        LineDirectiveRec const* best = nullptr;
        for (auto const& rec : it->second) {
            // No early `break`: a full scan returns the last-applicable record
            // regardless of append order, matching `presumedFile`'s reverse scan.
            // Records ARE appended in increasing order today (one forward pass
            // per origin), but that invariant lives in SourceBuffer, not here —
            // an include/buffer cache would break it silently, and the two
            // lookups would then disagree. ~50 records for lemon's parse.c.
            if (rec.physLine < physLine) best = &rec;
        }
        return best;
    }

    // PRESUMED line for a physical line: N + (physLine - directiveLine - 1).
    [[nodiscard]] std::uint32_t presumedLine(void const* origin,
                                             std::uint32_t physLine) const {
        LineDirectiveRec const* const rec = activeLineDir(origin, physLine);
        if (rec == nullptr) return physLine;
        return rec->presumedLine + (physLine - rec->physLine - 1u);
    }

    // PRESUMED file name. C23 6.10.4p3: the file operand is OPTIONAL and when
    // omitted the presumed name is left UNCHANGED — so walk back to the most
    // recent record that actually CARRIED a name, and leave `name` untouched if
    // none did. (A `#line N` after a `#line M "f"` keeps reporting "f".)
    void presumedFile(void const* origin, std::uint32_t physLine,
                      std::string& name) const {
        auto const it = lineDirs_.find(origin);
        if (it == lineDirs_.end()) return;
        for (auto r = it->second.rbegin(); r != it->second.rend(); ++r) {
            if (r->physLine >= physLine) continue;
            if (r->hasFile) { name = r->file; return; }
        }
    }

    std::vector<Token> materializePredefined(PredefinedMacroDef const& def,
                                             ByteOffset invOffset) {
        switch (def.kind) {
        case PredefinedMacroKind::Line: {
            // C 6.10.8.1: the LINE number of the macro's INVOCATION -- but the
            // PRESUMED one, which `#line` may have remapped (C23 6.10.4p2-3).
            // Resolve the invocation offset through the line-map to its ORIGIN
            // buffer + offset, read the 1-based PHYSICAL line, then apply any
            // active `#line` for that origin. Null/empty line-map -> line 1
            // (defensive; a real TU always has a map).
            std::uint32_t line = 1;
            if (lineMap_ != nullptr && !lineMap_->empty()) {
                LineMap::Resolved const r = lineMap_->resolve(invOffset);
                if (r.origin != nullptr) {
                    line = r.origin->lineCol(r.offset).line;
                    line = presumedLine(r.origin, line);
                }
            }
            return materializeSignificant(std::to_string(line));
        }
        case PredefinedMacroKind::File: {
            // C 6.10.8.1: the PRESUMED NAME of the current source file. Resolve
            // the invocation offset to its ORIGIN buffer so a `__FILE__` inside an
            // `#include`'d header reports the HEADER's name, not the main file's,
            // then let an active `#line "file"` override it (C23 6.10.4p3 -- the
            // file operand is OPTIONAL, and when omitted the presumed name is
            // left UNCHANGED, so the override only applies when one was given).
            // `\` -> `/` normalized, then quoted as a C string literal.
            std::string name = "<source>";   // defensive synth name
            if (lineMap_ != nullptr && !lineMap_->empty()) {
                LineMap::Resolved const r = lineMap_->resolve(invOffset);
                if (r.origin != nullptr) {
                    name = std::string{r.origin->name()};
                    presumedFile(r.origin, r.origin->lineCol(r.offset).line, name);
                }
            }
            for (char& c : name) {
                if (c == '\\') c = '/';
            }
            return materializeSignificant(quoteCString(name));
        }
        case PredefinedMacroKind::Counter: {
            // D-CSUBSET-COUNTER-MACRO-NOT-EXPANDED. `__COUNTER__`: a per-TU
            // monotonically increasing decimal that advances ONCE PER EXPANSION.
            //
            // ★★ THIS IS THE ONLY STATEFUL KIND, AND THAT IS THE WHOLE POINT OF
            // GIVING IT A KIND RATHER THAN A `constant` ROW. Every other kind is a
            // pure function of its argument — Line/File of the invocation OFFSET,
            // Date/Time of a value fixed at construction — so a config could in
            // principle carry them. A counter cannot BE a string in the config:
            // its value depends on how many times it has already been read.
            //
            // ★ PER-TRANSLATION-UNIT RESET IS STRUCTURAL, NOT A `reset()` SOMEONE
            // HAS TO REMEMBER TO CALL: the counter is a member of THIS expander,
            // `preprocessRun` constructs exactly one expander per translation
            // unit, and it is the only expansion path that materializes a
            // predefined value at all (the include PRE-SCAN consults definedness
            // only, never a value). So two TUs in one invocation cannot disagree
            // about a name they both mint.
            //
            // ★ POST-INCREMENT — the FIRST expansion yields 0, matching both
            // references. ✔MEASURED with gcc 13.3.0 and clang 18.1.3 / 19.1.1:
            // all three start at 0 and step by 1. Nothing observable should depend
            // on the specific integer (the construct's whole purpose is UNIQUENESS
            // of the pasted identifier), but agreeing costs nothing and removes a
            // reason for a real program's assumption to be surprised — so the
            // answer is recorded here rather than left to be re-litigated.
            return materializeSignificant(std::to_string(counter_++));
        }
        case PredefinedMacroKind::Constant:
            // A static integer-constant spelling carried verbatim.
            return materializeSignificant(def.value);
        case PredefinedMacroKind::Date:
            return materializeSignificant(quoteCString(dateString_));
        case PredefinedMacroKind::Time:
            return materializeSignificant(quoteCString(timeString_));
        }
        return {};   // unreachable (the kind set is closed); silence warnings
    }

    // Wrap `s` in a C string literal: surround with `"` and backslash-escape each
    // interior `"` and `\` (C 6.4.5). Used for the `__FILE__`/`__DATE__`/`__TIME__`
    // string-literal products. (A normalized file name or the date/time spellings
    // contain neither in practice, but escaping is exact + future-proof.)
    static std::string quoteCString(std::string_view s) {
        std::string out = "\"";
        for (char const c : s) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    // FC15a (A2 -- the load-bearing buffer mechanism): MATERIALIZE a `#`/`##`
    // product whose spelling is `spelling`. The spelling is APPENDED to
    // `productText_` (which `preprocess()` later concatenates onto the synth text
    // before freezing the FINAL buffer), then re-tokenized; each resulting
    // SIGNIFICANT token (non-trivia, non-Eof) is returned with its span REWRITTEN
    // to point at the appended region of the (eventual) final buffer
    // (`prefixLen_ + productBase + tokenOffset`). So a product token slices to its
    // real spelling -- `add3` / `"hello"` -- from the SAME buffer the parser
    // parses, never to `##`/`#`. The re-tokenization uses a throwaway reporter so
    // a malformed product does not pollute the user diagnostics here (the caller's
    // F1/F2 logic owns the user-facing fail-loud).
    // ── [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]] ────────────────────────────
    //
    // Record where a just-minted product run CAME FROM, into the same line map
    // that owns every other synth coordinate. Called from the ONE place product
    // bytes are appended, so a run can never exist without provenance and a new
    // minting site cannot forget to register.
    //
    // ⚠ ASCENDING BY CONSTRUCTION: `productText_` is append-only (its own note
    // says so — a product span is the absolute `prefixLen_ + offset`, never
    // rewound), so successive calls hand `LineMap::addExpansion` strictly
    // increasing `productStart`s, which is the order `expansionFor` requires.
    void recordProductProvenance(ByteOffset base, ByteOffset end) {
        if (lineMap_ == nullptr || end <= base) return;
        MacroExpansionSite site;
        site.productStart  = prefixLen_ + base;
        site.productEnd    = prefixLen_ + end;
        site.siteOffset    = mint_.site;
        site.defOffset     = mint_.def;
        site.hasDefinition = mint_.hasDef;
        site.name          = mint_.name;
        lineMap_->addExpansion(std::move(site));
    }

    std::vector<Token> materializeSignificant(std::string_view spelling) {
        const ByteOffset productBase =
            static_cast<ByteOffset>(productText_.size());
        productText_.append(spelling);
        recordProductProvenance(productBase,
                                static_cast<ByteOffset>(productText_.size()));
        auto tiny = SourceBuffer::fromString(std::string{spelling}, "<pp-product>");
        DiagnosticReporter scratch;
        auto ppToks = tokenizeToPP(tiny, schema_, scratch);
        // ── [[D-PP-PASTE-REJECTS-A-VALID-PREPROCESSING-NUMBER]]: THE RETIMING ──
        //
        // The scratch reporter used to be a DROP: a malformed product was judged
        // by the preprocessor's own token COUNT instead, and a pp-number the
        // language's literal grammar does not accept came back as two tokens and
        // was refused as "not a single valid token". Phase-3 scanning now returns
        // ONE token for it, so the count no longer refuses — and the refusal has
        // to arrive from the tier that actually knows, which is the scanner that
        // just said `P_MalformedNumber`.
        //
        // ★ REPOSITIONED ONTO THE MINT SITE, never left on `<pp-product>`. The
        // tiny buffer dies at the end of this function and its offsets name no
        // file; `mint_.site` is the EXPANSION SITE the sibling row already
        // records, so the diagnostic lands where the user wrote the invocation
        // and the ordinary remap carries it home. ✔MEASURED, gcc 13.3.0 and
        // clang 18.1.3 both report the invalid-suffix error at the invocation for
        // exactly this construct.
        // ⚠ ONLY `P_MalformedNumber`, and the narrowing is load-bearing rather
        // than cautious. The scratch reporter also holds artefacts of tokenizing
        // an ISOLATED FRAGMENT — a `1"x"` product ends INSIDE a string, so the
        // scan reports `P_UnterminatedString` at the tiny buffer's EOF — and
        // those say nothing about the program. ✔MEASURED: forwarding the whole
        // reporter made DSS refuse `L ## "s"`, a paste gcc 13.3.0 and clang
        // 18.1.3 both accept, which is the SAME bar violation this row exists to
        // remove, arrived at from the opposite direction. The "is this ONE
        // preprocessing token" question still belongs to the token COUNT below;
        // exactly one judgement moved tiers, and this is it.
        for (ParseDiagnostic const& d : scratch.all()) {
            if (d.code != DiagnosticCode::P_MalformedNumber) continue;
            ParseDiagnostic moved = d;
            moved.buffer = synth_->id();
            moved.span   = SourceSpan::empty(mint_.site);
            rep_.report(std::move(moved));
        }
        std::vector<Token> out;
        for (PPToken const& pt : ppToks) {
            if (isTrivia(pt.tok) || isNewline(pt.tok)) continue;
            if (pt.tok.coreKind == CoreTokenKind::Eof) continue;
            Token t = pt.tok;
            // Rewrite the tiny-buffer span into the final-buffer product region.
            t.span = SourceSpan::of(
                prefixLen_ + productBase + pt.tok.span.start(),
                prefixLen_ + productBase + pt.tok.span.end());
            out.push_back(t);
        }
        return out;
    }

    // FC15a (F1, C 6.10.3.3p3): build the single TOKEN produced by pasting the
    // spelling `spelling` (the left operand's spelling concatenated with the
    // right's). The pasted text MUST re-tokenize to EXACTLY ONE significant token
    // -- `lookupLexeme` alone is insufficient (it would silently accept `1##"x"`
    // -> `1"x"` or `)##(` -> `)(`). Zero or more-than-one significant tokens is a
    // constraint violation -> fail loud P_PreprocessorPaste (positioned at the
    // `##`), returning nullopt; the caller recovers by emitting both operands.
    std::optional<Token> pasteTokens(std::string_view spelling,
                                     SourceSpan opSpan) {
        // NOTE: materializeSignificant appends to productText_ unconditionally,
        // so a REJECTED paste still occupies a few bytes of the product tail --
        // harmless (no token references them; the buffer only grows).
        std::vector<Token> toks = materializeSignificant(spelling);
        if (toks.size() != 1) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPaste, synth_->id(), opSpan,
                   std::string{"pasting formed '"} + std::string{spelling}
                       + "', which is not a single valid token");
            return std::nullopt;
        }
        return toks.front();
    }

    // Union of an argument token's own (accreted) hide set with the invocation
    // hide set `hs`. Argument tokens were pre-expanded in the caller's context,
    // so they may already hide names; we ADD `hs` (the invoked macro plus the
    // surviving name∩close intersection) on top, never dropping the argument's.
    static HideSet hideUnionAll(HideSet const& argHide, HideSet const& hs) {
        if (!hs || hs->empty()) return argHide;
        if (!argHide || argHide->empty()) return hs;
        auto out = std::make_shared<std::set<std::string>>(*argHide);
        out->insert(hs->begin(), hs->end());
        return out;
    }

    // Stream macro-expander (C 6.10.3 / 6.10.3.4 via Prosser's PRECISE per-token
    // hide set). Walks a cursor over the ExpToken working set `in`; on each
    // expansion it SPLICES the substituted tokens (each carrying its computed
    // hide set) back over the consumed `[i, past)` region of `in` and RESCANS
    // from the splice point -- so a function-like name and a `(` that become
    // adjacent only ACROSS the boundary between a replacement and the surrounding
    // parent stream are re-paired (`A(F)(3)`, `NAME(4)`). A token whose macro name
    // is IN its own hide set is frozen (direct self-reference, mutual recursion).
    // The `depth` backstop is defense-in-depth: a correct hide set already bounds
    // recursion, but a malformed/over-deep chain still fails LOUD here rather than
    // downstream at the parser.
    std::vector<ExpToken> expand(std::vector<ExpToken> in, int depth) {
        std::vector<ExpToken> out;
        if (depth > 256) {                 // pathological-NESTING backstop
            // FAIL LOUD (FC13 cycle 2 review fold): emit a positioned
            // diagnostic at the backstop instead of silently returning the
            // input verbatim. A silently-truncated deep nest otherwise fails
            // DOWNSTREAM at the parser with an inscrutable error; surfacing it
            // HERE attributes the real cause (macro expansion nested too deep)
            // to the PP. Position on the first token of this run (the deepest
            // re-entry's lead token) when available.
            //
            // Under the PRECISE hide set a finite macro CHAIN (`M0`->...->`Mn`)
            // expands ITERATIVELY in one frame (each step splices + rescans,
            // depth stays flat) and terminates correctly -- so this backstop no
            // longer fires on a finite chain (that was a cycle-2 artifact of the
            // recursive engine). What still recurses is NESTING: argument
            // pre-expansion (`expand(arg, depth+1)`), so a pathological
            // 256-deep-nested argument (`F(F(F(...F(0)...)))`) trips this guard.
            // Defense-in-depth: the hide set already bounds macro recursion; this
            // catches an over-deep nest (or an internal bug) loudly, not silently.
            emitPP(rep_, DiagnosticCode::P_PreprocessorUnsupported, synth_->id(),
                   (in.empty() ? SourceSpan::empty(0) : in.front().tok.span),
                   "macro expansion nesting too deep (>256)");
            truncated_ = true;   // stream is now truncated — PP fatal
            return in;
        }
        // D-PERF-1: the working stream is a FRONT-CONSUMED deque -- the cursor is
        // ALWAYS the front. The loop consumes `work` strictly front-to-back and
        // every splice happens AT the front (`spliceOver(work, 0, ...)`), so a
        // pop_front + push_front is O(consumed + repl) per expansion instead of the
        // O(n) mid-vector tail-shift the old `std::vector` cursor paid PER
        // expansion -> the O(n^2) macro pass is gone. `out` still accumulates the
        // passed-over tokens IN ORDER (byte-identical output). (The backstop above
        // still reads/returns the vector `in`, so its truncation semantics stay
        // exactly as before; `in` is moved-FROM here and not touched again.)
        std::deque<ExpToken> work(std::make_move_iterator(in.begin()),
                                  std::make_move_iterator(in.end()));
        // TF-C82: THE emission chokepoint. Every token that leaves this frame is
        // stamped with the `#pragma pack` cap in effect AT THAT MOMENT — which is
        // what makes a `_Pragma("pack(4)")` sitting mid-run split the run
        // correctly, and what makes a composite arriving from a macro replacement
        // carry the cap of its INVOCATION rather than of its `#define` line.
        // Only at depth 0: a depth>0 frame is ARGUMENT pre-expansion, whose
        // tokens are spliced back and re-emitted through this same chokepoint by
        // the parent. `recordPack_` keeps the `#if`-operand expansions (which
        // reach no parser) out of the map.
        auto emitOut = [&](ExpToken&& e) {
            if (depth == 0 && recordPack_ && !e.placemarker) {
                notePackForToken(e.tok);
                // TF-C85: the `#pragma optimize` region rides the SAME
                // chokepoint under the SAME gate. Both are lexically scoped
                // pragmas whose product is keyed on EMISSION, so sharing the
                // gate is what keeps them from drifting apart — a second
                // stamping site with its own depth/`recordPack_` conditions is
                // exactly how one of the two would silently stop tracking macro
                // invocations.
                noteNoOptimizeForToken(e.tok);
            }
            out.push_back(std::move(e));
        };
        while (!work.empty()) {
            // Audit fix #2 (UAF ordering): COPY the front token before any
            // pop/splice below -- `t.tok`, `t.hide`, `t.invOffset`, `t.tok.span`
            // and `name` must all stay valid across the pop_front/push_front that
            // `spliceOver` (or the emit-then-pop arms) perform on `work`. A
            // REFERENCE into `work.front()` would dangle the instant the front is
            // popped.
            ExpToken t = work.front();
            // ★★ D-PP-DEFINED-VIA-MACRO-EXPANSION — THE OPERAND BARRIER, and it
            // sits HERE, before the non-Word early-out, because the barrier has
            // to SEE the `(` and `)` to know where the operand is.
            //
            // Depth 0 ONLY: a depth>0 frame is ARGUMENT pre-expansion, and the
            // references pre-expand a `defined`-bearing ARGUMENT unprotected —
            // ✔MEASURED, `#define ID(a) a` + `#if ID(defined(FOO))` is an ERROR
            // in gcc 13.3.0 AND clang 18.1.3 ("operator \"defined\" requires an
            // identifier" / "macro name must be an identifier"), because `FOO`
            // becomes `1` before substitution. Arming the barrier there would
            // make DSS ACCEPT what no reference accepts, which §A.3b forbids in
            // exactly the same words as failing on what one accepts.
            //
            // A protected token is copied VERBATIM: not macro-expanded, not
            // materialized as a predefined, and never paired with a following
            // `(` as a function-like invocation (`#if defined(F)` for a
            // function-like `F` answers 1 on all three references).
            if (ifDefinedBarrier_ != nullptr && depth == 0) {
                bool const isProtected = ifDefinedBarrier_->protects(
                    t.tok, isWord(t.tok) ? text(t.tok) : std::string_view{});
                // ★★ D-PP-DEFINED-VIA-MACRO-EXPANSION — THE DIAGNOSTIC, and the
                // AND that decides it is made of two facts each owner already
                // holds: the BARRIER says "that token was the `defined`
                // keyword", and the HIDE SET says "that token arrived from a
                // replacement list". A token lifted from the directive line
                // carries an EMPTY hide set (`fromToken`); every replacement
                // token carries `hideAdd(t.hide, name)` and so is non-empty.
                // Neither fact is re-derived here and no token flag was minted
                // for it.
                //
                // C 6.10.1 leaves the construct UNDEFINED and DSS evaluates it
                // anyway, because the union of the references does — but all
                // three references SAY SO while doing it (gcc/clang
                // `-Wexpansion-to-defined`, MSVC `C5105`), and evaluating in
                // silence is the one behaviour none of them has. Warning
                // severity: `errorCount()` is untouched, translation continues,
                // and `--suppress` can silence it (deliberately NOT in the
                // unsuppressable closed table — see the NEGATIVE pin there).
                //
                // Reachability is free here rather than argued: `expandTokens`
                // is only ever called to evaluate a LIVE `#if`/`#elif` operand,
                // so a macro-produced `defined` in a NOT-TAKEN branch is never
                // expanded and never diagnosed (C 6.10p1) — the invariant the
                // `#error`/`#warning`/`#pragma` arms state explicitly.
                if (ifDefinedBarrier_->lastWasDefinedKeyword()
                    && t.hide != nullptr && !t.hide->empty()) {
                    emitPP(rep_,
                           DiagnosticCode::P_PreprocessorDefinedFromExpansion,
                           synth_->id(), t.tok.span,
                           "the 'defined' operator here was produced by macro "
                           "expansion; C 6.10.1 leaves the result undefined "
                           "(gcc, clang and MSVC all evaluate it, and all three "
                           "warn)",
                           DiagnosticSeverity::Warning);
                }
                if (isProtected) {
                    emitOut(std::move(t));
                    work.pop_front();
                    continue;
                }
            }
            if (!isWord(t.tok)) {
                emitOut(std::move(t));
                work.pop_front();
                continue;
            }
            const std::string name{text(t.tok)};
            // TF-C82 (`_Pragma`; C 6.10.9) — checked BEFORE the macro table, so a
            // (constraint-violating) `#define _Pragma …` can never shadow the
            // operator, and checked HERE rather than at the directive scan so a
            // `_Pragma` reached through a macro REPLACEMENT LIST resolves at its
            // expansion site. `pragmaOperator` is config: empty (toy / tsql) makes
            // `_Pragma` an ordinary identifier and this arm unreachable.
            if (!cfg().pragmaOperator.empty() && name == cfg().pragmaOperator
                && consumePragmaOperator(work, t)) {
                continue;
            }
            auto it = table_.find(name);
            // Not a `#define`d macro, OR M is in THIS token's hide set (Prosser:
            // M ∉ hideset(T) required to expand).
            if (it == table_.end() || hideContains(t.hide, name)) {
                // FC15b (predefined macros; C 6.10.8): on a `#define` table MISS,
                // an identifier that is a PREDEFINED-macro name materializes its
                // configured value (`__LINE__`/`__FILE__`/`__STDC__`/...). The
                // value token(s) are spliced over `[i, i+1)` and RESCANNED (a
                // Number/Constant rescans inertly; a string-literal pair likewise)
                // -- so the resolved value reaches the parser exactly like any
                // other replacement. Resolved against THIS token's invocation
                // offset, so a `__LINE__` arriving via a macro replacement reports
                // the INVOCATION line. The hide set is irrelevant to a predefined
                // name (it is never self-referential). Gated on a genuine `#define`
                // table MISS (`it == table_.end()`), so a (constraint-violating,
                // fail-loud) shadowing `#define` could never reach this arm.
                if (it == table_.end()) {
                    auto pit = predefined_.find(name);
                    if (pit != predefined_.end()) {
                        // [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: a predefined
                        // macro's value is minted too, and it has NO definition
                        // site — there is no `#define` line to point at, and
                        // saying so is what keeps `hasDefinition` from meaning
                        // "offset zero". The POSITION is still exact: the
                        // invocation, like every other product.
                        MintScope const mintScope{*this, t.invOffset, 0,
                                                  /*hasDef=*/false, name};
                        std::vector<Token> value =
                            materializePredefined(pit->second, t.invOffset);
                        std::vector<ExpToken> repl;
                        repl.reserve(value.size());
                        // A predefined product is replacement-origin: carry THIS
                        // token's invocation offset (inert for the value kinds,
                        // but kept consistent with every other product path).
                        for (Token const& v : value) {
                            repl.push_back(ExpToken{v, t.hide, t.invOffset});
                        }
                        // The value stands where the macro NAME stood -> it keeps that
                        // name's leading spacing (a `__LINE__` reached through a
                        // stringized argument must not lose the space before it).
                        inheritLeadingSpacing(repl, t.spacedBefore);
                        spliceOver(work, 0, 1, repl);
                        continue;   // rescan from the materialized value
                    }
                }
                emitOut(std::move(t));
                work.pop_front();
                continue;
            }
            MacroDef const& def = it->second;
            // [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: everything this
            // invocation mints — a `##` product on either arm, a `#` product on
            // the function-like one — is attributed to the INVOCATION (`t`, an
            // original-source anchor propagated through every nesting level) and
            // to THIS macro's definition. One scope over both arms, so neither
            // can acquire a minting path the other's stamp does not cover.
            MintScope const mintScope{*this, t.invOffset, def.definitionSite,
                                      def.hasDefinitionSite, name};
            if (!def.isFunctionLike) {
                // OBJECT-like (Prosser): replace T with M's replacement, each
                // token carrying hideset(T) ∪ {M}; splice over [i, i+1) and
                // RESCAN from i (so a function-like name newly exposed at the
                // replacement's tail re-pairs with the parent's `(`).
                const HideSet hs = hideAdd(t.hide, name);
                std::vector<ExpToken> repl;
                repl.reserve(def.replacement.size());
                // FC15b: a replacement token INHERITS the invoking name token's
                // invocation offset, so a `__LINE__` reached via an object-like
                // macro (`#define WARN __LINE__`) resolves to the INVOCATION line,
                // not the `#define` line.
                // Adjacency INSIDE the replacement list comes from the `#define`
                // line's own spans (writer (2) of `spacedBefore`): trivia was dropped
                // at definition time, but the list is one contiguous run of that line,
                // so a gap between consecutive tokens is exact. ✔MEASURED: with
                // `#define OBJ p+q` / `#define OBJS p + q`, a two-level stringize
                // yields "p+q" and "p + q" respectively on all three oracles.
                for (std::size_t k = 0; k < def.replacement.size(); ++k) {
                    Token const& r = def.replacement[k];
                    ExpToken e{r, hs, t.invOffset};
                    e.spacedBefore =
                        k > 0
                        && def.replacement[k - 1].span.end() != r.span.start();
                    repl.push_back(e);
                }
                // FC15 paste residuals (D-PP-PASTE-OBJECT-LIKE, C 6.10.3.3): `##`
                // applies to OBJECT-like macros too. Route the replacement through
                // the SAME `collapsePastes` chokepoint the function-like path uses
                // (no duplicated paste logic). An object-like macro has no parameters,
                // so no placemarker can arise here; `collapsePastes` collapses the
                // literal `##` operators and still fail-louds a genuine dangling `##`
                // (`#define OBJ a ##`). `#` (stringize) does NOT apply to object-like
                // macros (C 6.10.3.2) and there is none to handle here.
                repl = collapsePastes(std::move(repl), hs, t.invOffset,
                                      /*sweepPlacemarkers=*/true);
                // The replacement stands where the NAME stood -> its first token keeps
                // the name's leading spacing, not the define line's (whose first token
                // has no predecessor and is therefore always `false`).
                inheritLeadingSpacing(repl, t.spacedBefore);
                spliceOver(work, 0, 1, repl);
                continue;          // rescan from i (the first replacement token)
            }
            // FUNCTION-like: an invocation ONLY if the next significant token is
            // the configured `(`. Otherwise emit the name VERBATIM (C 6.10.3p10:
            // a function-like name not followed by `(` is not an invocation).
            std::size_t openIdx = nextSignificant(work, 1);
            if (openIdx >= work.size() || !isParenOpen(work[openIdx].tok)) {
                emitOut(std::move(t));
                work.pop_front();
                continue;
            }
            std::size_t past = 0;
            std::vector<ExpToken> separators;  // depth-1 commas (for __VA_ARGS__)
            HideSet closeHide;                 // close-paren hide set (Prosser ∩)
            auto argsOpt =
                collectArgs(work, openIdx, name, past, separators, closeHide);
            if (!argsOpt) {
                // Unterminated invocation already reported: emit the name as-is
                // and resume after it (do NOT swallow the rest of the stream).
                emitOut(std::move(t));
                work.pop_front();
                continue;
            }
            std::vector<std::vector<ExpToken>> args = std::move(*argsOpt);
            // Zero-PARAMETER normalization (C 6.10.3p4): a NON-variadic macro that
            // takes NO parameters invoked as `M()` is ZERO arguments, but
            // collectArgs reports it as one EMPTY argument (the general groups
            // rule). Collapse that one empty group to zero args ONLY when the
            // macro declares no parameters, so `M()` matches arity 0 while a
            // one-parameter `G()` keeps its single empty argument. For a VARIADIC
            // macro with zero NAMED params (`V(...)`) invoked as `V()`, the same
            // collapse yields zero arguments -> an EMPTY variadic portion (C23 ok)
            // -- so the named-arity floor (0) is met and __VA_ARGS__ is empty.
            if (def.params.empty() && args.size() == 1 && args[0].empty()) {
                args.clear();
            }
            // ARITY check (C 6.10.3p4). NON-variadic: exact match. VARIADIC: at
            // least `params.size()` arguments (the named params); the rest --
            // possibly NONE (C23 allows an empty variadic part) -- form
            // __VA_ARGS__. Mismatch -> fail loud, emit the name verbatim, skip
            // the whole call.
            const bool arityBad = def.isVariadic
                                      ? (args.size() < def.params.size())
                                      : (args.size() != def.params.size());
            if (arityBad) {
                emitPP(rep_, DiagnosticCode::P_PreprocessorMacroArgument,
                       synth_->id(), t.tok.span,
                       std::string{"function-like macro "} + name
                           + (def.isVariadic ? " expects at least "
                                             : " expects ")
                           + std::to_string(def.params.size())
                           + " argument(s) but got "
                           + std::to_string(args.size()));
                // Audit fix #1 (the silent-miscompile seam): emit the name, then
                // pop `past` tokens TOTAL off the front -- the name at index 0 PLUS
                // the malformed `(...)` in `[1, past)` -- pushing NOTHING back. The
                // old vector code did `i = past` (advance the cursor past the whole
                // malformed call). In the deque model that MUST become an explicit
                // pop of `past` front tokens; leaving it a no-op (or popping only
                // the name) would re-scan/re-expand the malformed args -> a SILENT
                // divergence from the byte-identical output.
                emitOut(std::move(t));
                for (std::size_t k = 0; k < past; ++k) work.pop_front();
                continue;
            }
            // The Prosser function-like hide set:
            //   HS' = (hideset(name) ∩ hideset(close-paren)) ∪ {M}.
            // Intersecting with the CLOSE paren's hide set is what keeps a
            // self-reference frozen (`F(x) F(x)`: the rescanned inner `F` and its
            // `)` both carry {F}, so F stays hidden) WHILE letting a cross-stream
            // re-pairing expand (`A(F)(3)`: the `(3)` came from the parent with an
            // EMPTY hide set, so the intersection drops {A} and F is free).
            const HideSet hs = hideAdd(hideIntersect(t.hide, closeHide), name);
            // PRE-EXPAND each NAMED argument FULLY before substitution
            // (C 6.10.3.1): arguments expand in the CURRENT context (their tokens
            // keep whatever hide sets they already carry; the invoked macro is NOT
            // yet added -- that happens at substitution via `hs`). For a variadic
            // macro, only the first `params.size()` args bind to named params; the
            // rest are gathered into __VA_ARGS__.
            const std::size_t namedCount =
                def.isVariadic ? def.params.size() : args.size();
            // FC15a (C 6.10.3.2 / 6.10.3.3p1): a `#`/`##` OPERAND uses the RAW
            // (un-pre-expanded) argument. Capture each named arg's raw tokens AND
            // the raw trailing `__VA_ARGS__` run BEFORE the pre-expansion move
            // below consumes `args[k]`. `substitute` chooses raw-vs-expanded per
            // operand position. (Cheap: only the call's own arg tokens; the common
            // no-`#`/`##` macro never reads these.)
            std::vector<std::vector<ExpToken>> rawArgs;
            rawArgs.reserve(namedCount);
            for (std::size_t k = 0; k < namedCount; ++k) rawArgs.push_back(args[k]);
            std::vector<ExpToken> rawVaArgs;
            if (def.isVariadic) {
                for (std::size_t k = def.params.size(); k < args.size(); ++k) {
                    if (k > def.params.size() && k - 1 < separators.size()) {
                        rawVaArgs.push_back(separators[k - 1]);
                    }
                    rawVaArgs.insert(rawVaArgs.end(), args[k].begin(),
                                     args[k].end());
                }
            }
            std::vector<std::vector<ExpToken>> expandedArgs;
            expandedArgs.reserve(namedCount);
            for (std::size_t k = 0; k < namedCount; ++k) {
                expandedArgs.push_back(expand(std::move(args[k]), depth + 1));
            }
            // Build the (pre-expanded) __VA_ARGS__ token run from the TRAILING
            // arguments (indices >= params.size()), each pre-expanded like a
            // named arg, re-joined with the ORIGINAL source separator commas
            // (`separators[k]` separates arg k from arg k+1). EMPTY when the
            // variadic portion is empty (C23). A non-variadic macro passes an
            // empty run (substitute never consults it).
            std::vector<ExpToken> vaArgs;
            if (def.isVariadic) {
                for (std::size_t k = def.params.size(); k < args.size(); ++k) {
                    if (k > def.params.size()) {
                        // The separator BEFORE this trailing arg is the comma
                        // between arg (k-1) and arg k == separators[k-1].
                        if (k - 1 < separators.size()) {
                            vaArgs.push_back(separators[k - 1]);
                        }
                    }
                    std::vector<ExpToken> ex =
                        expand(std::move(args[k]), depth + 1);
                    vaArgs.insert(vaArgs.end(), ex.begin(), ex.end());
                }
            }
            // FC15b: the WHOLE call's replacement-origin tokens inherit the
            // invoking NAME token's invocation offset (`invOffset`), so a
            // `__LINE__` in a function-like replacement resolves to the
            // invocation line. (`t` is captured before the splice below.)
            const ByteOffset callInvOffset = t.invOffset;
            std::vector<ExpToken> substituted =
                substitute(def, expandedArgs, vaArgs, rawArgs, rawVaArgs, hs,
                           callInvOffset);
            // The whole call `[i, past)` is replaced by this run, so the run's first
            // token keeps the spacing that preceded the macro NAME. ✔MEASURED: a
            // two-level stringize of `a PLAIN(1,2)` is "a g(1, 2)" and of
            // `a+PLAIN(1,2)` is "a+g(1, 2)" on all four oracles.
            inheritLeadingSpacing(substituted, t.spacedBefore);
            // Splice the substituted result over the WHOLE call `[i, past)` and
            // RESCAN from i: the invoked macro M is in every substituted token's
            // hide set, so a self-reference is frozen; a function-like name newly
            // exposed at the substitution's tail re-pairs with the parent's `(`.
            spliceOver(work, 0, past, substituted);
            continue;   // rescan the substitution + the trailing parent stream
        }
        return out;
    }

    // Replace `in[from, to)` with `repl` (the freshly produced tokens) and leave
    // the cursor implicitly at `from` (== the FRONT) for a rescan. D-PERF-1: the
    // stream is a FRONT-CONSUMED deque and the cursor is ALWAYS the front, so
    // every call site passes `from == 0`. Pop `[from, to)` off the front, then
    // push `repl` at the front in REVERSE so `repl[0]` becomes the new front (the
    // rescan continues there). FRONT ops only -> O(repl + consumed) per expansion,
    // NOT the O(n) mid-vector tail-shift the old vector erase+insert paid PER
    // expansion -> the O(n^2) macro pass is gone. Non-`static` so it can bump the
    // effectiveness counter. (`from` is always 0; the loop-form pop keeps this
    // general + correct — no hard assert that could fire in a release build.)
    void spliceOver(std::deque<ExpToken>& in, std::size_t from, std::size_t to,
                    std::vector<ExpToken> const& repl) {
        for (std::size_t k = from; k < to; ++k) in.pop_front();
        for (auto it = repl.rbegin(); it != repl.rend(); ++it) in.push_front(*it);
        tokenMoves_ += (to - from) + repl.size();
    }

    std::shared_ptr<SourceBuffer>        synth_;
    std::shared_ptr<GrammarSchema const> schema_;
    DiagnosticReporter&                  rep_;
    // Set TRUE when the >256 macro-expansion-nesting backstop fires and
    // RETURNS the input verbatim (truncating the expansion). Surfaced via
    // `truncated()` so `preprocess()` can flag the result fatal.
    bool                                 truncated_ = false;
    // D-PERF-1: accumulated FRONT-splice token-move count (see `tokenMoves()`).
    std::size_t                          tokenMoves_ = 0;
    // D-PP-DEFINED-VIA-MACRO-EXPANSION: non-null ONLY for the duration of one
    // `#if`/`#elif` controlling-expression expansion (armed by `expandTokens`,
    // torn down by its RAII guard). While armed, `expand`'s depth-0 frame asks it
    // whether the token it is about to process is the OPERAND of a `defined`, and
    // copies it VERBATIM when it is. Null in the body pass, so `defined` there is
    // an ordinary identifier and nothing changes.
    PpIfOperandBarrier*                    ifDefinedBarrier_ = nullptr;
    SchemaTokenId                        hashKind_{};
    SchemaTokenId                        parenOpen_{};
    SchemaTokenId                        parenClose_{};
    SchemaTokenId                        argSep_{};
    SchemaTokenId                        variadicMarker_{};
    // FC15a: the `#`/`##` operator kinds (config-resolved; InvalidSchemaToken
    // when the language declares neither -- then the engine never produces a
    // product).
    SchemaTokenId                        stringizeKind_{};
    SchemaTokenId                        pasteKind_{};
    // FC17.9(h): the QUOTE-include opener kind (StringStart) + the ANGLE opener
    // kind (the reused hasIncludeAngleOpenToken = LtOp) that `handleEmbed` matches
    // to extract the `#embed "resource"` filename / detect the deferred angle form
    // (InvalidSchemaToken when the language declares no `#embed`).
    SchemaTokenId                        quoteIncludeKind_{};
    SchemaTokenId                        embedAngleOpenKind_{};
    // FC15a (A2): byte length of the PREFIX buffer (`synth_`), and the
    // accumulated `#`/`##` PRODUCT spellings appended AFTER it. A product token's
    // span is `[prefixLen_ + offsetInProductText_, ...)`; `text()` dispatches a
    // span at-or-after `prefixLen_` to `productText_`. The final synth buffer is
    // `synth_->text() + productText_` (built by `preprocess()` after `run()`).
    ByteOffset                           prefixLen_{};
    std::string                          productText_;
    // ── [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: WHO IS MINTING RIGHT NOW ──
    //
    // The construct whose expansion is currently producing product bytes, so
    // `materializeSignificant` — the ONE chokepoint every minted byte passes
    // through — can stamp each run with where it came from.
    //
    // ★ A SCOPED MEMBER RATHER THAN A PARAMETER, AND THE REASON IS THE SHAPE OF
    // THE ENGINE. `materializeSignificant` is reached from ten call sites across
    // `substitute` / `collapsePastes` / `stringizeTokens` / `materializePredefined`
    // / `handleEmbed` / `consumePragmaOperator`; threading a parameter through all
    // of them puts the same fact in ten signatures and lets a future eleventh site
    // forget it silently. The mint is always SYNCHRONOUS inside one of those
    // operations, so a scope guard is exact — and `MintScope` SAVES AND RESTORES,
    // which is what keeps argument pre-expansion (`expand(arg, depth + 1)`) from
    // leaking its context into the parent's substitution.
    //
    // ⚠ `site` is the OUTERMOST source anchor (`ExpToken::invOffset`, propagated
    // down through every nested and chained expansion), while `name`/`def` name
    // the INNERMOST macro — the one whose replacement list holds the `#`/`##`.
    // That is deliberate and it is what clang prints: the error at the invocation
    // in the user's file, the note at the definition that minted the token.
    struct MintContext {
        ByteOffset  site   = 0;
        ByteOffset  def    = 0;
        bool        hasDef = false;
        std::string name;
    };
    MintContext                          mint_;
    class MintScope {
    public:
        MintScope(MacroExpander& owner, ByteOffset site, ByteOffset def,
                  bool hasDef, std::string name)
            : owner_(owner), saved_(owner.mint_) {
            owner_.mint_ = MintContext{site, def, hasDef, std::move(name)};
        }
        MintScope(MintScope const&)            = delete;
        MintScope& operator=(MintScope const&) = delete;
        ~MintScope() { owner_.mint_ = std::move(saved_); }

    private:
        MacroExpander& owner_;
        MintContext    saved_;
    };
    std::unordered_map<std::string, MacroDef> table_;
    // FC15b (predefined macros; C 6.10.8): the config-seeded predefined-macro
    // set (name -> def), the synth-offset -> origin line-map (for the
    // offset-derived `__LINE__`/`__FILE__`; may be null/empty for a no-include
    // identity pass), and the once-computed `__DATE__`/`__TIME__` INNER spellings
    // (without the surrounding quotes -- `materializePredefined` quotes them).
    std::unordered_map<std::string, PredefinedMacroDef> predefined_;
    LineMap*                             lineMap_ = nullptr;
    std::string                          dateString_;   // "Mmm dd yyyy"
    std::string                          timeString_;   // "hh:mm:ss"
    // FC15c (`__has_include`; C23 6.10.1p4): the include search paths +the main
    // file's own directory, so the operator's existence test resolves a header
    // EXACTLY as the include machinery would. `includeDirs_` = quote-form search
    // (self-dir first); `systemDirs_` = angle-form `<stem>.json` search. Empty
    // for a test caller that passes none -> `__has_include` then finds nothing on
    // the path (only an absolute/self-dir hit), which is the correct answer for
    // that input.
    std::span<fs::path const>            includeDirs_;
    std::span<fs::path const>            systemDirs_;
    std::optional<ObjectFormatKind>      activeFormat_;
    // D-PP-HEADER-CASE-INSENSITIVE-PE: the active format's header-NAME case
    // rule, applied by every include search the AUTHORITATIVE pass performs.
    HeaderNameMatching                   headerNameMatching_;
    fs::path                             includingDir_;
    // FC14: the conditional-compilation frame stack (one frame per open
    // `#if`/`#ifdef`/`#ifndef`). See CondFrame + handleIf/Elif/Else/Endif.
    std::vector<CondFrame>               condStack_;
    // c17 (D-PP-CONDITIONAL-INCLUDE-ORDERING, authoritative dead-regions): the
    // `[start,end)` byte ranges of the synth (prefix) buffer that fall in a DEAD
    // conditional branch, as determined by THIS authoritative pass's
    // `stackActive()` (the full `table_`+`predefined_` macro state). `run()`
    // records them as it crosses conditional directives; `preprocess()` consults
    // them to suppress a provisional `P_IllegalChar` whose source byte is dead.
    // Because the verdict comes from the SAME liveness the compiler emits/skips
    // tokens on, the illegal-char oracle can never diverge from the real branch
    // decision -- the silent-miscompile class the pre-scan oracle had (it could
    // not see predefined/header macros) is gone by construction.
    std::vector<std::pair<ByteOffset, ByteOffset>> deadRanges_;
    // ── TF-C82 (D-PP-PRAGMA-REGISTRY): the `#pragma pack` state ──
    //
    // `packCurrent_` is the maximum member alignment in effect RIGHT NOW, in
    // bytes; 0 = no cap (the target's natural alignment rules, i.e. every
    // composite laid out exactly as before this cycle). `packStack_` is the
    // `pack(push, N)` / `pack(pop)` stack. ONE stack serves BOTH spellings:
    // `#pragma pack` mutates it from `handleDirective`, `_Pragma("pack(...)")`
    // from `expand`, and because `run()` is a SINGLE ordered walk over the token
    // stream those two mutation sites interleave in genuine source order without
    // any merge step.
    std::vector<std::uint32_t>           packStack_;
    std::uint32_t                        packCurrent_ = 0;
    // True only during the ONE expansion whose output reaches the parser (see
    // `flushExpand`). Keeps `#if`-operand expansions out of `packByOffset_`.
    bool                                 recordPack_  = false;
    // The product: synth byte offset of an EMITTED token -> the pack cap in
    // effect when it was emitted. SPARSE — only tokens under a NON-ZERO cap get
    // an entry, so a TU with no `#pragma pack` (every existing one) produces an
    // EMPTY map and costs nothing. The semantic tier looks up a composite
    // specifier's FIRST TOKEN offset here.
    //
    // ★ WHY PER-TOKEN AND NOT A SORTED LIST OF BYTE REGIONS. A region list is
    // keyed on where the pragma SITS; this is keyed on where a token was
    // EMITTED, and the two disagree for exactly the case that matters most:
    // a composite arriving from a macro REPLACEMENT LIST carries the `#define`
    // line's byte span, which is nowhere near the `pack(4)` region containing
    // the INVOCATION. A region lookup would answer "no cap" and lay the struct
    // out wrong, in silence. Stamping at emission answers with the state that
    // was actually in effect. The one case a per-token map cannot express — the
    // SAME source token emitted twice under DIFFERENT caps — is detected at
    // insert and fails loud rather than letting last-writer-wins pick a layout.
    std::unordered_map<std::uint32_t, std::uint32_t> packByOffset_;

    // ── TF-C85: the `#pragma optimize` state (the `optimizerControl` verb) ──
    //
    // `optimizeOff_` is TRUE between a `#pragma optimize("", off)` and the
    // matching `("", on)`. Unlike `pack` this state has NO stack: MSVC's
    // `#pragma optimize` is a flat on/off switch, not a push/pop discipline
    // (`optimize("", on)` restores the COMMAND-LINE setting, it does not pop a
    // saved one), so modelling it with a stack would invent a nesting rule the
    // source language does not have.
    bool optimizeOff_ = false;
    // The product: the synth byte offsets of tokens EMITTED while `optimizeOff_`
    // was set. SPARSE and STICKY — an offset is inserted once and never removed,
    // so a TU with no `#pragma optimize` (every one before this cycle) produces
    // an EMPTY set and costs nothing.
    //
    // ★ WHY STICKY RATHER THAN A `kAmbiguous` SENTINEL LIKE `pack`'s. The SAME
    // source token CAN reach the output twice under different states (a macro
    // whose replacement list is expanded both inside and outside a region) — the
    // shape that forced `pack` to record an ambiguity and have the semantic tier
    // judge it. Here that machinery would be dishonest ceremony, because the two
    // candidate answers are NOT symmetric: `pack` has no safe default (either
    // layout may be the wrong ABI), whereas every DSS optimizer pass is
    // semantics-preserving, so resolving a contested token to OFF can only cost
    // optimization, never correctness. Sticky-off IS that resolution, taken at
    // the only place that can see the conflict. It is silent DELIBERATELY: there
    // is no wrong artifact to warn about.
    std::unordered_set<std::uint32_t> noOptimizeByOffset_;
};

} // namespace

// The pre-scan work counters. See `PreScanMemoCounters` in preprocessor.hpp for
// why the property [[D-PERF-PP-EVERY-INCLUDE-RE-READS-AND-RE-TOKENIZES-THE-SAME-HEADER]]
// pins is observed as a COUNT and not as elapsed time.
//
// ⚠ THE TWO LOADS ARE NOT TAKEN TOGETHER, and no caller may treat them as a
// consistent pair. On a THREAD POOL another unit can build or hit between them,
// so `builds + hits` is not "requests at an instant" — it is two independent
// accounting totals. Locking them into a snapshot would put the include path's
// mutex on a read that exists only to be reported.
PreScanMemoCounters::Row PreScanMemoCounters::read() noexcept {
    Row r;
    r.builds = preScanBuildCount().load(std::memory_order_relaxed);
    r.hits   = preScanHitCount().load(std::memory_order_relaxed);
    return r;
}

void PreScanMemoCounters::reset() noexcept {
    preScanBuildCount().store(0, std::memory_order_relaxed);
    preScanHitCount().store(0, std::memory_order_relaxed);
}

// FC17.9(h) (D-PP-EMBED, the streaming boundary): the PURE size-budget check
// (declared in preprocessor.hpp for direct unit-testability). Returns a
// user-facing diagnostic message when a resource's byte count would blow the
// cycle-1 non-streaming splice budget, else nullopt. The handler emits the
// message as `P_PreprocessorEmbed` on the directive word -- a catchable LOUD
// wall, never an OOM.
std::optional<std::string> embedResourceSizeError(std::size_t byteCount) {
    if (byteCount <= kEmbedMaxResourceBytes) return std::nullopt;
    return std::string{"#embed resource is "} + std::to_string(byteCount)
        + " bytes, over the " + std::to_string(kEmbedMaxResourceBytes)
        + "-byte cycle-1 splice budget; the non-streaming splice materializes "
          "~2 tokens/byte and would exhaust memory (see D-PP-EMBED-STREAMING)";
}

// ── TF-C74 + TF-C97: the ONE merge of the language, target and format
//    predefine lists ─────────────────────────────────────────────────────
//
// See `preprocessor.hpp` for the contract. The three invariants worth restating
// where the code is:
//
//   • the collision scan runs BEFORE the format filter. A language `_WIN32`
//     gated `["pe"]` and a target `_WIN32` ungated are a collision on EVERY
//     leg, including elf/macho where the language entry is filtered out — the
//     fault is that two configs claim the same NAME, and if it only surfaced
//     on the pe leg a maintainer could ship the conflict and never see it.
//
//   • the scan covers ALL THREE ORDERED PAIRS, and that is why the families are
//     a TABLE here rather than three hand-written loops. TF-C97 added a third
//     family; with two, one loop was the whole scan, and the natural way to add
//     a third is to write one more loop against the language — which silently
//     leaves target×format unscanned. Enumerating pairs over a table makes
//     "every pair" structural, so a FOURTH family is one row and not a fresh
//     chance to miss a pair.
//
//   • the format filter runs exactly ONCE, right here. `effective` is the only
//     predefine list that leaves this function, and every seed site consumes
//     it verbatim.
//
// NOTE (agnosticism): nothing in this function knows any macro's name, any
// architecture's name or any object format's name. It compares config-supplied
// strings to each other and never to a literal; the only literals are the JSON
// POINTER PATHS of the three config families, which name FILES to edit and are
// what makes the diagnostic actionable.
MergedPredefinedMacros mergePredefinedMacros(
    std::span<PredefinedMacroDef const> languageMacros,
    std::span<PredefinedMacroDef const> targetMacros,
    std::span<PredefinedMacroDef const> formatMacros,
    std::optional<ObjectFormatKind>     activeFormat,
    std::span<PredefinedMacroExclusionGroup const> exclusiveGroups) {
    MergedPredefinedMacros out;

    // The per-format availability predicate, in its ONE surviving location.
    // Local to this function on purpose: no seed site can call it.
    auto const availableHere = [&](PredefinedMacroDef const& pm) {
        if (pm.availableObjectFormats.empty()) return true;   // every format
        if (!activeFormat.has_value()) return false;          // no target ⇒ only universal
        return ffi::objectFormatInAvailabilitySet(pm.availableObjectFormats,
                                                  *activeFormat);
    };

    // The three families, in merge order. `label`+`path` exist ONLY to build
    // the diagnostic: a collision message that does not name both config files
    // leaves the maintainer to guess which of three to edit. ★ The path carries
    // the FILE FAMILY as well as the JSON pointer because the target and format
    // families use the SAME pointer (`/predefinedMacros`) — a bare pointer
    // would name two different files identically, which is precisely the
    // "which file do I edit?" failure the paths are here to prevent.
    struct Family {
        std::span<PredefinedMacroDef const> macros;
        std::string_view                    label;
        std::string_view                    path;
    };
    // The PATHS come from the SHARED `kPredefinedMacroFamilyPaths` table (see
    // preprocessor.hpp): the `requires` satisfaction check names the same files
    // in its own diagnostics, and a second copy of these strings is a copy that
    // can disagree about which config file the author must open.
    std::array<Family, 3> const families{
        Family{languageMacros, "language", kPredefinedMacroFamilyPaths[0]},
        Family{targetMacros,   "target",   kPredefinedMacroFamilyPaths[1]},
        Family{formatMacros,   "format",   kPredefinedMacroFamilyPaths[2]}};

    // (a) COLLISION SCAN — pre-filter, so gating cannot hide a conflict, over
    // every unordered pair of families (i < j).
    for (std::size_t i = 0; i < families.size(); ++i) {
        for (std::size_t j = i + 1; j < families.size(); ++j) {
            for (PredefinedMacroDef const& a : families[i].macros) {
                for (PredefinedMacroDef const& b : families[j].macros) {
                    if (a.name != b.name) continue;
                    out.conflicts.push_back(std::format(
                        "predefined macro '{}' is declared by BOTH the {} "
                        "config ({}) and the {} config ({}) — neither "
                        "declaration may silently win; remove one, or rename "
                        "it so the two configs own distinct names",
                        a.name, families[i].label, families[i].path,
                        families[j].label, families[j].path));
                    break;
                }
            }
        }
    }
    // Within-list duplicates are rejected at LOAD (`parsePredefinedMacroArray`),
    // so a name can appear at most once per family and the scan above is
    // sufficient. Returning early keeps `effective` empty — there is no
    // partially-merged state a caller could mistake for usable.
    if (!out.conflicts.empty()) return out;

    // (b)+(c) FILTER ONCE, stable order: language, then target, then format.
    out.effective.reserve(languageMacros.size() + targetMacros.size()
                          + formatMacros.size());
    for (Family const& fam : families) {
        for (PredefinedMacroDef const& pm : fam.macros) {
            if (availableHere(pm)) out.effective.push_back(pm);
        }
    }
    // (e) D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC — THE MUTUAL-EXCLUSION PIN.
    //
    // Runs on the FILTERED list on purpose (see the header's property (e)): the
    // defect is a pair that is individually legitimate and only contradictory
    // once a format makes both effective, so before the filter there is no
    // offending format to name.
    //
    // AGNOSTIC: not one macro name, language, architecture or object format is
    // written here. The groups are whatever the language config declared, and
    // the format is printed through the shared `objectFormatKindName`.
    for (PredefinedMacroExclusionGroup const& grp : exclusiveGroups) {
        std::vector<std::string> present;
        for (std::string const& want : grp.macros) {
            auto const hit = std::ranges::find(out.effective, want,
                                               &PredefinedMacroDef::name);
            if (hit != out.effective.end()) present.push_back(want);
        }
        if (present.size() < 2) continue;
        std::string names;
        for (std::string const& n : present) {
            if (!names.empty()) names += "' and '";
            names += n;
        }
        out.conflicts.push_back(std::format(
            "predefined macros '{}' are ALL effective for object format {} — "
            "they are declared mutually exclusive and at most one may be "
            "defined for any one target. {}. Gate or remove all but one "
            "(`availableObjectFormats` on the offending entry), or drop the "
            "group from `preprocess.mutuallyExclusivePredefinedMacros` if the "
            "rule itself is wrong",
            names,
            activeFormat.has_value() ? objectFormatKindName(*activeFormat)
                                     : std::string_view{"<none>"},
            grp.reason));
    }
    // A group violation makes `effective` unusable for exactly the reason the
    // name-collision arm above does — a TU would be preprocessed under an
    // identity no reference compiler can present — so the SAME contract holds:
    // non-empty `conflicts` means the caller must abort, and there is no
    // "merge anyway" mode.
    if (!out.conflicts.empty()) out.effective.clear();

    // (d) empty `targetMacros` + empty `formatMacros` leaves exactly the
    // format-filtered language-only list the pre-TF-C74 engine built at each
    // seed site.
    return out;
}

// c105 (D-PP-USER-DEFINE): the ONE owner of the `--define NAME[=VALUE]` split
// and of its `VALUE defaults to 1` rule. See the header for why the three
// callers (the `<command-line>` prologue, the include-gating pre-scan prefix,
// and `--dump-predefined-macros`) must not each carry their own copy.
//
// FIRST `=`, deliberately: a macro VALUE may legitimately contain `=`
// (`--define EQ_OP="a == b"`), while a NAME may not — so the first separator is
// the only split that cannot truncate a value. (`--resolve-library` splits on
// the LAST `=` for the mirror-image reason; see `CliArgs::resolveLibraries`.)
//
// Agnosticism: compares `entry` against exactly one character and one string
// literal, the latter being the DEFAULT VALUE the C/gcc `-D` rule specifies.
// Nothing here knows a macro name, a language, an architecture or a format.
UserDefineSplit splitUserDefine(std::string_view entry) noexcept {
    auto const eq = entry.find('=');
    if (eq == std::string_view::npos) {
        // No `=` at all. `1` is the value gcc's `-DNAME` supplies and the value
        // this project documents on `CliArgs::defines`.
        return UserDefineSplit{entry, std::string_view{"1"},
                               /*valueWasStated*/ false};
    }
    // Everything after the first `=`, EMPTY INCLUDED: `--define NAME=` states an
    // EMPTY value, which is a different macro from `--define NAME`. Folding the
    // empty case back onto the default would silently rewrite the user's request.
    return UserDefineSplit{entry.substr(0, eq), entry.substr(eq + 1),
                           /*valueWasStated*/ true};
}

// FC15b (C 6.10.8.1): the translation DATE + TIME spellings, from ONE read of the
// wall clock. Extracted from the `MacroExpander`'s private `computeDateTime` so
// `--dump-predefined-macros` can report the SAME spellings a real TU gets instead
// of re-deriving the C-mandated formats — a second derivation of a format is how a
// dump ends up disagreeing with the compile it claims to describe.
//
// Agnosticism: no language / architecture / object-format appears; the only
// literals are the C standard's own month abbreviations and field layouts.
TranslationTimestamp translationTimestamp() {
    TranslationTimestamp out;
    const std::time_t now = std::time(nullptr);
    // ★★ THREAD-SAFE BY CONSTRUCTION — `std::localtime` IS NOT, AND THIS IS NOW
    // CALLED CONCURRENTLY. `std::localtime` returns a pointer into a SHARED
    // process-wide `std::tm`, so two translation units expanding `__DATE__` /
    // `__TIME__` at the same instant read a buffer the other is overwriting —
    // a silent wrong-spelling race, not a crash. Since D-PERF-4 the driver
    // builds the front half of every TU on a thread pool, so that is a real
    // interleaving rather than a theoretical one, and c declares BOTH
    // macros (`c.lang.json`), so the path is live for every C compile.
    // The reentrant spelling fills a CALLER-OWNED `tm`; the `_WIN32` / POSIX
    // split is pure host portability (which name the host libc gives the
    // reentrant function), never a target or format identity.
    std::tm       tmBuf{};
    const std::tm* lt = nullptr;
#ifdef _WIN32
    if (::localtime_s(&tmBuf, &now) == 0) lt = &tmBuf;
#else
    lt = ::localtime_r(&now, &tmBuf);
#endif
    // Defensive: a null `localtime` leaves BOTH strings empty -> a synth `""`
    // literal and a dump that says the value is empty, never a fabricated date.
    if (lt == nullptr) return out;
    {
        static char const* const kMon[12] = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun",
            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const int mon = (lt->tm_mon >= 0 && lt->tm_mon < 12) ? lt->tm_mon : 0;
        // Sized for the format's WORST CASE over arbitrary `int`, not for a
        // well-formed date: `%s`(3) + ' ' + `%2d`(<=11, "-2147483648") + ' '
        // + `%04d`(<=11) + NUL = 28. Truncation is therefore structurally
        // impossible rather than merely improbable — `tm_mon` is range-checked
        // just above, but `tm_mday` and `tm_year` are not, so a hostile/broken
        // `localtime` could otherwise silently truncate `__DATE__` to a wrong
        // spelling (snprintf would not overflow, so this is a correctness bug,
        // never a memory one).
        // ★ Surfaced by gcc-13 -Wformat-truncation on an aarch64 host, which
        // inlines this far enough to see the bound; the x86_64 legs do not
        // report it. D-PP-DATETIME-BUF-SIZED-FOR-VALID-TM-ONLY.
        char buf[32];
        // SPACE-padded day (`%e` is not portable on MSVC), 4-digit year.
        std::snprintf(buf, sizeof(buf), "%s %2d %04d", kMon[mon],
                      lt->tm_mday, lt->tm_year + 1900);
        out.date = buf;
    }
    {
        // Same worst-case sizing: three `%02d` at <=11 each + 2 ':' + NUL = 36.
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", lt->tm_hour,
                      lt->tm_min, lt->tm_sec);
        out.time = buf;
    }
    return out;
}

namespace {

// The preprocess RUN — the whole pass, MINUS the result-shape guarantee.
// PRECONDITION (validated by the exported `preprocess()` wrapper below, which
// is this function's ONLY caller): `mainSource` and `schema` are non-null and
// `schema->preprocess().enabled` is true.
//
// ★ WHY THIS IS NOT THE EXPORTED ENTRY POINT
//   ([[D-PP-RESULT-CONTRACT-SINGLE-EXIT]]).
// `PreprocessResult` documents an invariant its consumers rely on: `tokens` is
// Eof-terminated and `synthBuffer` is live. The happy path establishes that
// invariant at its very TAIL (the Eof append + `SourceBuffer::fromString`
// below). So the invariant held only for the path that ran to completion, and
// EVERY early return silently shipped a result that broke the documented shape
// — with no way for the caller to tell, because the shape is asserted in a
// comment.
//
// That was not hypothetical. The predefined-macro-collision abort returned with
// `tokens` EMPTY and `synthBuffer` NULL, and `compilation_unit.cpp`'s
// `pp.tokens.back()` — carrying the comment "Eof-terminated by contract" —
// dereferenced past the end of an empty vector. MEASURED TF-C115: the CLI
// SEGFAULTED (rc 139 Linux / 0xC0000005 Windows) with NOT ONE diagnostic
// printed, for a fault whose entire purpose is to be loud.
//
// Wrapping the run funnels every `return` — this one and every one a future
// cycle adds — through ONE exit that establishes the contract. A new early
// return can no longer reintroduce the crash by omission, which is the whole
// point: the previous shape made "remember to Eof-terminate" a rule each
// author had to know, and the fix makes it a rule no author can break.
PreprocessResult preprocessRun(
    std::shared_ptr<SourceBuffer>        mainSource,
    std::shared_ptr<GrammarSchema const> schema,
    std::span<fs::path const>            includeDirs,
    HeaderNameMatching                   headerNameMatching,
    DiagnosticBudget                     budget,
    std::span<fs::path const>            systemDirs,
    std::optional<ObjectFormatKind>      activeFormat,
    std::span<std::string const>         userDefines,
    std::span<PredefinedMacroDef const>  targetPredefinedMacros,
    std::span<PredefinedMacroDef const>  formatPredefinedMacros) {
    PreprocessResult result;
    result.diagnostics = std::make_unique<DiagnosticReporter>(budget.asConfig());

    // TF-C74 + TF-C97: merge the LANGUAGE, TARGET and FORMAT predefined-macro
    // lists ONCE, here, and apply the per-format filter ONCE while doing it.
    // Every one of the four downstream seed sites reads `merged.effective` and
    // nothing else — that is what makes the pre-scan/authoritative agreement
    // structural rather than a convention four sites have to keep.
    MergedPredefinedMacros const merged = mergePredefinedMacros(
        schema->preprocess().predefinedMacros, targetPredefinedMacros,
        formatPredefinedMacros, activeFormat,
        schema->preprocess().mutuallyExclusivePredefinedMacros);
    if (!merged.conflicts.empty()) {
        // EITHER a name owned by more than one config, OR
        // (D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC)
        // a mutually exclusive group with more than one
        // member effective on this format. Both are the same failure in the
        // end — the TU would be preprocessed under an identity that is not a
        // real one — so neither may silently win and the pass does not run at
        // all: `fatal` stops the caller from treating the token stream as a
        // successful preprocess.
        //
        // This return produces NO tokens and NO synth buffer. That is fine
        // HERE and only here: the single exit in `preprocess()` turns it into
        // the well-formed EMPTY translation unit the contract promises. Before
        // TF-C115 this return went straight to the caller and crashed it —
        // see `establishResultContract` for what "well-formed empty" means and
        // why "refused to run" is representable rather than a hole.
        for (std::string const& msg : merged.conflicts) {
            emitPP(*result.diagnostics,
                   DiagnosticCode::C_ConflictingPredefinedMacro,
                   mainSource->id(), SourceSpan::empty(ByteOffset{0}), msg);
        }
        result.fatal = true;
        return result;
    }

    std::string synthText;

    // c105 (D-PP-FUNCTION-LIKE-PREDEFINE + D-PP-USER-DEFINE): the synthetic
    // PROLOGUES, prepended to the synth stream BEFORE the main source so the
    // ORDINARY directive handler seeds them in stream order (the gcc model:
    // "as if #define appeared before the first source line"). Two origins:
    //   "<built-in>"     — config predefinedMacros WITH `params` (function-like,
    //                      e.g. the MSVC-profile `__declspec(x)` → empty erase),
    //                      format-filtered exactly like the predefined_ seed.
    //   "<command-line>" — the CLI `--define NAME[=VALUE]` entries (VALUE
    //                      defaults to 1). Because these become ORDINARY
    //                      macros, the handler gives for free: name validation,
    //                      the C 6.10.8.1 predefined-collision guard (a -D may
    //                      not silently flip `_MSC_VER`), the 6.10.3p2
    //                      duplicate policy, and #undef-ability.
    // Each prologue is its own SourceBuffer so line-mapped diagnostics point
    // at the synthetic origin by name. Empty prologues append nothing — the
    // synth stream is byte-identical to the pre-c105 shape.
    {
        std::string builtinText;
        // TF-C74: the EFFECTIVE list (seed site #3 of four) — already
        // format-resolved, so no filter is re-applied here. A TARGET-declared
        // function-like predefine lowers to a "<built-in>" `#define` exactly
        // like a language-declared one.
        // D-PP-PREDEFINE-REDEFINITION-PARTITION: the prologue now also carries
        // every OBJECT-like row whose config says a program may redefine it.
        // That is not a relaxation bolted onto the refusal — it is the SAME
        // mechanism the references use, in their own words: gcc reports a
        // redefined compiler-identity macro at `<command-line>` over a
        // `<built-in>` definition, and clang's diagnostic literally reads
        // `In file included from <built-in>:388:`. ✔MEASURED 2026-08-26 (both,
        // probed separately). Lowering an implementation predefine to an
        // ordinary `#define` here is therefore the reference architecture, and
        // every behaviour the partition owes falls out of the directive handler
        // that already exists: `#undef` composes, an incompatible redefinition
        // warns and the new definition wins (D-PP-INCOMPATIBLE-REDEFINITION-IS-FATAL,
        // itself measured against those same references), an IDENTICAL
        // redefinition is silent per 6.10.5p2, and `#ifdef` tracks all of it.
        for (PredefinedMacroDef const& pm : merged.effective) {
            // A WARN row stays in the `predefined_` name set, where the two
            // diagnostics live. It must never be lowered here: a `#define` line
            // for a derived kind would shadow the ENGINE-COMPUTED value with a
            // static one, and lowering an ISO-listed constant would turn the
            // program's own `#define` of it into an ordinary redefinition — the
            // wrong diagnostic, from the wrong rule.
            // ⚠ THE `isFunctionLike` CLAUSE IS NOT REDUNDANT, even though the
            // loader refuses a function-like row that is not `ordinary`. This
            // site must agree with the `predefined_` seed, which skips EVERY
            // function-like row unconditionally — and a `PredefinedMacroDef`
            // built in C++ (tests, and any future programmatic producer) never
            // passes through that loader, so it carries the struct's default
            // verb. Keyed on the verb alone, such a row would be in NEITHER
            // map: silently not predefined at all. Structural agreement here
            // costs one clause; relying on a validator two tiers away to keep
            // an invariant that this loop could just state is how a hole opens.
            if (!pm.isFunctionLike
                && predefinedNameIsDiagnosedOnChange(pm.programRedefinition)) {
                continue;
            }
            builtinText += "#define ";
            builtinText += pm.name;
            if (pm.isFunctionLike) {
                builtinText += '(';
                for (std::size_t i = 0; i < pm.params.size(); ++i) {
                    if (i != 0) builtinText += ',';
                    builtinText += pm.params[i];
                }
                builtinText += ')';
            }
            builtinText += ' ';
            builtinText += pm.value;
            builtinText += '\n';
        }
        if (!builtinText.empty()) {
            auto origin = SourceBuffer::fromString(builtinText, "<built-in>");
            appendWithContinuationSplice(origin->text(), origin, 0, synthText,
                                         result.lineMap);
        }
        std::string cliText;
        for (std::string const& d : userDefines) {
            // `splitUserDefine` is the ONE owner of the NAME/VALUE split and of
            // the `VALUE defaults to 1` rule — shared with the pre-scan prefix
            // below and with `--dump-predefined-macros`.
            UserDefineSplit const s = splitUserDefine(d);
            cliText += "#define ";
            cliText += s.name;
            cliText += ' ';
            cliText += s.value;
            cliText += '\n';
        }
        if (!cliText.empty()) {
            auto origin = SourceBuffer::fromString(cliText, "<command-line>");
            appendWithContinuationSplice(origin->text(), origin, 0, synthText,
                                         result.lineMap);
        }
    }

    std::vector<core::PathIdentity> includeStack;
    includeStack.push_back(
        core::PathIdentity::of(fs::path{mainSource->name()}));
    // D-PP-PRAGMA-RECOGNIZED-SEMANTICS: the TU's include-once set. Its lifetime
    // is EXACTLY this translation unit — a fresh set per `preprocess()` call, so
    // one TU's `#pragma once` can never suppress another TU's include. The main
    // source is deliberately NOT seeded: `#pragma once` in the main file records
    // itself when the pragma is REACHED, exactly as it does in a header.
    IncludeOnceRegistry includeOnce;
    // C21 (D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE, Option 2): the `#define NAME
    // VALUE\n` VALUE prefix for the include-gating pre-scan. So a `#if
    // <cmdline/predefined>` VALUE guard (`#if SQLITE_TEST >= 1`,
    // `#if __STDC_VERSION__ >= 201112L`) gating a quote-`#include` evaluates
    // correctly, the pre-scan must see the macro's VALUE -- not just its
    // definedness. Built from:
    //   (a) every command-line `--define`, parsed by the SAME `splitUserDefine`
    //       the `<command-line>` prologue above uses (VALUE defaults to 1) -- so
    //       the pre-scan is more-live only IN LOCKSTEP with the authoritative
    //       pass (the one-directional-divergence invariant that keeps P0016
    //       closed). ★ Sharing the FUNCTION, not merely the intent: these two
    //       sites previously each carried their own copy of the split, held in
    //       agreement by this comment alone, and a divergence here silently
    //       changes whether a `#if NAME`-gated quote-`#include` resolves; PLUS
    //   (b) every OBJECT-like predefined macro available on the active format, via
    //       the SHARED filter (FINDING-B) the authoritative `predefined_` seed +
    //       `sbNameDefined` use -- so the sets cannot drift. FINDING-A: FUNCTION-
    //       like predefines (`isFunctionLike`) are EXCLUDED -- a bare `#if NAME`
    //       (no call) must fold to 0 in the pre-scan exactly as in the authoritative
    //       pass; value-seeding one would make the pre-scan MORE-live -> a silent
    //       P0016 re-open.
    // Each SynthBuilder prepends this as a NON-EMITTED span-safe scanBuf prefix (see
    // build()). Function-scope: it outlives every (recursive) SynthBuilder, held by
    // const-ref + threaded into children.
    std::string preScanDefinePrefix;
    for (std::string const& d : userDefines) {
        UserDefineSplit const s = splitUserDefine(d);
        preScanDefinePrefix += "#define ";
        preScanDefinePrefix += s.name;
        preScanDefinePrefix += ' ';
        preScanDefinePrefix += s.value;
        preScanDefinePrefix += '\n';
    }
    // TF-C74: the EFFECTIVE list (seed site #4 of four) — already
    // format-resolved, so no filter is re-applied. This is what makes a
    // per-architecture predefine usable as a VALUE guard on a quote-`#include`
    // (`#if __aarch64__` … `#include "x"`), not merely as a definedness test.
    for (PredefinedMacroDef const& pm : merged.effective) {
        if (pm.isFunctionLike) continue;   // FINDING-A: never value-seed a call macro
        preScanDefinePrefix += "#define ";
        preScanDefinePrefix += pm.name;
        preScanDefinePrefix += ' ';
        preScanDefinePrefix += pm.value;
        preScanDefinePrefix += '\n';
    }
    // c17 (D-PP-CONDITIONAL-INCLUDE-ORDERING): the SynthBuilder is conditional-
    // aware ONLY to gate quote-`#include` splicing (a dead-branch quote include
    // must not resolve -- the P0016 fix). The dead-region byte set used to
    // suppress a dead-branch `P_IllegalChar` (the P000E fix) is NOT produced here:
    // it comes from the AUTHORITATIVE `MacroExpander` pass below (`deadRanges()`),
    // whose liveness sees the full macro table (predefined + header-supplied), so
    // the illegal-char oracle can never diverge from the real branch decision.
    // D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: each system descriptor the SynthBuilder
    // SPLICES for an angle `#include <h>`, paired with the synth-buffer byte offset
    // of the splice point. The splice is UN-GATED (it fires for a dead-branch include
    // too); the closure block below DROPS any pair whose offset lies in an
    // AUTHORITATIVE dead range (`expander.deadRanges()`), leaving exactly the
    // authoritatively-live set the finish() oracle's `shippedLibDescriptors` holds.
    // Threaded by reference into the SynthBuilder (and its recursive children).
    std::vector<std::pair<fs::path, ByteOffset>> resolvedParents;
    // TF-C60 (D-PP-PRESCAN-CROSS-BUFFER-MACRO-STATE): the ROOT owns the pre-scan
    // macro map; every child builder threads it by reference, so `#define`s flow
    // across include boundaries in document order (both directions).
    std::unordered_map<std::string, SynthBuilder::SbMacro> preScanMacros;
    SynthBuilder builder{schema, includeDirs, systemDirs, activeFormat,
                         headerNameMatching,
                         *result.diagnostics, 0, includeStack, includeOnce,
                         result.fatal,
                         preScanDefinePrefix, merged.effective,
                         resolvedParents, preScanMacros};
    {
        // D-PERF-1 sub-timing: the synth-buffer splice (recursive concat of the
        // main file + every quote-#include, + the line-map). Nests under the
        // outer Preprocess scope, so its self-time is subtracted there.
        substrate::PhaseTimers::Scope ppSplice{
            substrate::CompilePhase::PreprocessSplice};
        builder.build(mainSource, synthText, result.lineMap);
    }

    // FC15a (A2 reorder): the `#`/`##` operators produce SYNTHETIC tokens whose
    // text does not exist in the spliced prefix. Their spelling must reach the
    // PARSER via a real span, so it is APPENDED to the synth text and the FINAL
    // buffer is frozen AFTER expansion. Until then, tokenization + the macro
    // expander read the spliced text via a PROVISIONAL PREFIX buffer (the same
    // bytes as the final buffer's leading prefix, so every original-token span
    // resolves identically in both). `prefixLen` is the byte length of that
    // prefix; a product token's span points at `[prefixLen + productOffset, ...)`.
    const ByteOffset prefixLen = static_cast<ByteOffset>(synthText.size());
    // [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: tell the coordinate map where the
    // MINTED tail begins, BEFORE the macro pass runs. Without this the map treats
    // every offset as a prefix offset and answers a product span by extrapolating
    // off the last segment — the exact wrong answer this row is about. Stamped at
    // the ONE place `prefixLen` is computed, next to the buffer it describes.
    result.lineMap.setProductBase(prefixLen);
    auto prefixBuffer = SourceBuffer::fromString(
        synthText, std::string{mainSource->name()});
    // (`result.mainSourceId` is stamped by `establishResultContract` on the
    //  single exit, so an aborting return carries it too — it used to be set
    //  here, i.e. on the happy path only.)

    // c17 (P000E fix): the main tokenize's diagnostics go to a PROVISIONAL
    // reporter, NOT straight onto `result.diagnostics`. A `P_IllegalChar` whose
    // source byte falls in a DEAD conditional branch (`#if 0 $ #endif`) must be
    // SUPPRESSED -- but only after the AUTHORITATIVE conditional pass has run and
    // recorded its dead byte-ranges. Every OTHER tokenizer diagnostic is forwarded
    // unconditionally below; a `P_IllegalChar` is promoted via the dead-region
    // oracle, keyed on the source BYTE's liveness (so a `$` consumed by an ACTIVE
    // `#define` line / `#`-stringize / an uninvoked LIVE macro body still reports;
    // a survival oracle keyed on "did the Error token reach the parser" would
    // wrongly drop those).
    DiagnosticReporter provisionalTokDiags{budget.asConfig()};
    auto ppToks = [&] {
        // D-PERF-1 sub-timing: the single tokenize of the synth buffer.
        substrate::PhaseTimers::Scope ppTok{
            substrate::CompilePhase::PreprocessTokenize};
        return tokenizeToPP(prefixBuffer, schema, provisionalTokDiags);
    }();
    std::vector<Token> synthTokens;
    synthTokens.reserve(ppToks.size());
    for (auto const& tk : ppToks) synthTokens.push_back(tk.tok);

    // FC15b: thread the synth-offset -> origin line-map into the expander so an
    // offset-derived predefined macro (`__LINE__`/`__FILE__`) resolves an
    // invocation offset to its real origin file + line.
    // FC15c: thread the include search paths + the main file's own directory so
    // `__has_include` resolves a header EXACTLY as the include machinery would
    // (quote form = self-dir + includeDirs; angle form = `<stem>.json` on
    // systemDirs).
    MacroExpander expander{prefixBuffer,  schema,      *result.diagnostics,
                           prefixLen,     &result.lineMap,
                           headerNameMatching,
                           includeDirs,   systemDirs,   activeFormat,
                           // The SHARED derivation — see
                           // `includingDirectoryOf`. `__has_include("h")` must
                           // give the SAME answer the `#include` above does, so
                           // it must derive its includer dir the same way.
                           includingDirectoryOf(mainSource->name()),
                           merged.effective};
    std::vector<Token> finalTokens;
    {
        // D-PERF-1 sub-timing: the macro pass (table build + stream expansion +
        // conditional elision) — the dominant preprocess stage on macro-heavy TUs.
        substrate::PhaseTimers::Scope ppExpand{
            substrate::CompilePhase::PreprocessExpand};
        finalTokens = expander.run(synthTokens);
        // D-PERF-1: surface the macro pass's front-splice token-move total (the
        // O(n^2)->O(n) effectiveness metric; a strict test asserts it <= k*N).
        result.macroTokenMoves = expander.tokenMoves();
    }
    // OR in the macro-expansion truncation; the SynthBuilder already wrote
    // `result.fatal` by reference for an include-nesting truncation.
    result.fatal = result.fatal || expander.truncated();

    // TF-C82 (D-PP-PRAGMA-REGISTRY): hand the `#pragma pack` stamps to the CU, so
    // the semantic tier can ask what alignment cap was in effect at a composite
    // specifier's first token. Empty for every TU that uses no `#pragma pack`.
    result.pragmaPackByOffset = expander.pragmaPackByOffset();
    result.pragmaNoOptimizeByOffset = expander.pragmaNoOptimizeByOffset();

    // c17 (authoritative dead-region oracle): promote the provisional tokenizer
    // diagnostics. A `P_IllegalChar` is forwarded to the real reporter UNLESS its
    // source byte (`span.start()`) lies in a DEAD conditional region as recorded
    // by the AUTHORITATIVE macro pass (`expander.deadRanges()`) -- so an illegal
    // char in a LIVE region reports no matter how its token is later consumed (a
    // `#define`-line `$`, a `#`-stringized `$`, an uninvoked live macro body),
    // including a branch the pre-scan could not evaluate but the full macro table
    // makes live (e.g. `#if __STDC__`); only a genuinely-dead one (`#if 0 $`) is
    // suppressed. ALL other tokenizer diagnostics forward unconditionally. The
    // span ids are unchanged (still the prefix buffer), so the later
    // `remapBuffers` re-homes them onto the final synth buffer exactly as before.
    // A byte offset is in an AUTHORITATIVE dead conditional region (`#if 0 …
    // #endif`) iff it falls in one of `expander.deadRanges()`. Those ranges are in
    // synthText coordinates (the expander ran over `prefixBuffer`, built from
    // `synthText`), so an offset recorded during the synth-buffer build maps
    // DIRECTLY. SHARED by the illegal-char oracle below AND the descriptor-seed
    // filter after it (D-PERF-2), so both read the SAME authoritative liveness.
    auto byteInDeadRegion = [&](ByteOffset b) {
        for (auto const& [ds, de] : expander.deadRanges()) {
            if (b >= ds && b < de) return true;
        }
        return false;
    };
    {
        // ── [[D-PP-SKIPPED-CONDITIONAL-GROUP-VALIDATED-AS-A-PHASE-7-NUMBER]] ──
        //
        // THIS GATE IS THE PHASE-7 BOUNDARY, and it used to name one code.
        // DSS scans the synth buffer ONCE, in phase 3, and hands those
        // preprocessing tokens straight to the parser — there is no second,
        // phase-7 scan for a preprocessed language. So every judgement phase 7
        // owes is made EAGERLY here, over text that includes groups phase 4 is
        // about to delete, and this loop is the only place that knows which of
        // them the conditional pass kept.
        //
        // The condition above read `d.code == P_IllegalChar`, with a comment
        // saying all other tokenizer diagnostics forward unconditionally. That
        // is not a rule about illegal characters — it is C 6.10.1p6, which says
        // a skipped group's text is divided into preprocessing tokens and *not
        // otherwise processed*, and it governs every phase-7 judgement equally.
        // Spelled as one code name it silently excluded the next one: when the
        // pp-number tail scan landed ([[D-PP-PASTE-REJECTS-A-VALID-PREPROCESSING-NUMBER]]),
        // `P_MalformedNumber` began firing on `%2d` inside upstream sqlite's
        // `#if 0` Tcl script and refused every translation unit that contains
        // one, on every host. The class now comes from
        // `isTokenConversionDiagnostic`, beside the codes themselves, so a third
        // conversion diagnostic inherits the rule instead of waiting for someone
        // to widen this line a second time.
        //
        // ⚠ THE ORACLE IS UNCHANGED AND STAYS THE BYTE'S LIVENESS, deliberately:
        // it is what keeps a `#if` CONTROLLING EXPRESSION loud. A controlling
        // directive's own line is outside the dead range (the range opens at the
        // line's END), and C 6.10.1p4 converts that line's preprocessing tokens
        // to tokens whether or not the expression evaluates them — ✔MEASURED,
        // gcc 13.3.0 and clang 18.1.3 both refuse `#if 0 && 2d`.
        for (ParseDiagnostic const& d : provisionalTokDiags.all()) {
            if (isTokenConversionDiagnostic(d.code)
                && byteInDeadRegion(d.span.start())) {
                continue;   // skipped group — divided into pp-tokens, not converted
            }
            result.diagnostics->report(d);
        }
    }

    // Append the accumulated `#`/`##` product spellings AFTER the prefix, then
    // freeze the FINAL buffer: ONE buffer whose unchanged leading prefix backs
    // every original token and whose appended tail backs every product token
    // (A2 -- no side-vector). When no product was generated this is byte-identical
    // to the prefix (the FC14 single-buffer behavior).
    synthText.append(expander.productText());
    result.synthBuffer = SourceBuffer::fromString(
        std::move(synthText), std::string{mainSource->name()});

    // Collect the DISTINCT origin buffers the line-map references (the original
    // main file + every spliced header), EXCLUDING the synth buffer. The
    // caller registers these so a `makeRemap`-redirected diagnostic resolves
    // for rendering instead of `--> <unknown-buffer:N>`. Dedup by buffer id.
    {
        std::unordered_set<BufferId> seenOrigins;
        BufferId const synthId = result.synthBuffer->id();
        for (LineMapSegment const& seg : result.lineMap.segments()) {
            if (!seg.origin) continue;
            BufferId const oid = seg.origin->id();
            if (oid == synthId) continue;   // never the synth buffer itself
            if (!seenOrigins.insert(oid).second) continue;
            result.originBuffers.push_back(seg.origin);
        }
    }

    // Diagnostics emitted during the build (BufferId{}, the synth id did not
    // exist yet) AND during tokenize/expansion (stamped with the PROVISIONAL
    // PREFIX buffer id) both belong on the FINAL synth buffer: every such span is
    // a prefix span, byte-identical in the final buffer (a strict prefix). Rewrite
    // both to the final synth id so the later `makeRemap` can attribute them.
    BufferId const prefixId = prefixBuffer->id();
    result.diagnostics->remapBuffers([&](BufferId& bid, SourceSpan&) {
        if (bid == BufferId{} || bid == prefixId) {
            bid = result.synthBuffer->id();
        }
    });

    Token eof;
    eof.coreKind = CoreTokenKind::Eof;
    eof.span     = SourceSpan::empty(
        static_cast<ByteOffset>(result.synthBuffer->size()));
    finalTokens.push_back(eof);
    result.tokens = std::move(finalTokens);

    // D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: turn the raw splice records into the
    // authoritatively-live descriptor set the first-parse typedef seed harvests.
    // (1) DROP any record whose splice offset lies in an AUTHORITATIVE dead range
    //     (`byteInDeadRegion`) -- the splice is UN-GATED (it fires for a dead-branch
    //     angle include too, D-PP-PRESCAN-ANGLE-MACRO-SPLICE-AUTHORITATIVE-LIVENESS),
    //     so THIS filter is what makes the surviving set == the finish() oracle's
    //     authoritatively-live `shippedLibDescriptors` (never a superset -> the seed
    //     can never resolve a name the reparse would not). An include the pre-scan
    //     mis-judged dead but the full macro table makes LIVE was still spliced +
    //     recorded, and its offset is NOT in a dead range, so it is KEPT (C30-safe).
    // (2) EXPAND each surviving PARENT to its TRANSITIVE `includes` closure and dedup
    //     CU-wide via the SHARED cycle-safe walker (`forEachDescriptorInClosure`, the
    //     same one the macro-splice + import-resolver tiers use), so the seed set can
    //     never disagree with the transitive surface actually injected. ONE `visited`
    //     set across every parent -> each descriptor appears once, keyed by weakly-
    //     canonical path. An `includes` entry that resolves to no descriptor is a
    //     config error the import resolver surfaces LOUD (F_ShippedHeaderNotFound on
    //     the `#include`); silent here to avoid a double-report.
    // EMIT-ONLY -- populates a new output field, changes no preprocess behavior.
    {
        std::unordered_set<core::PathIdentity> visited;
        for (auto const& [parent, off] : resolvedParents) {
            if (byteInDeadRegion(off)) continue;   // authoritatively-dead -> not seeded
            ffi::forEachDescriptorInClosure(
                parent, systemDirs, headerNameMatching, activeFormat, visited,
                [&](fs::path const& p) {
                    result.resolvedShippedDescriptors.push_back(p);
                },
                [](std::string const&, HeaderSearchResult const&) {
                    /* import resolver owns the loud miss AND the loud
                       fold-collision — both on the `#include` line */ },
                [](std::string const&, fs::path const&) {
                    /* import resolver owns the loud unavailable-child too. This
                       set feeds the typedef-name SEED, and the seed must never be
                       a SUPERSET of what `finish()` will resolve — passing the
                       same `activeFormat` the resolver uses is what keeps the two
                       sets identical rather than merely similar. */ });
        }
    }

    return result;
}

// Establish the `PreprocessResult` shape contract on the SINGLE exit every run
// funnels through ([[D-PP-RESULT-CONTRACT-SINGLE-EXIT]]).
//
// Two situations, deliberately handled DIFFERENTLY, because they are not the
// same fact:
//
//  (1) the run produced NO tokens at all. That is a LEGITIMATE abort state: the
//      pass refused to run (today: a predefined-macro collision, which must not
//      let either config silently win; tomorrow: whatever else must refuse).
//      The honest representation of "refused" is a well-formed EMPTY
//      translation unit — a real, empty synth buffer and the single Eof token
//      that terminates it — NOT a hole every consumer must remember to test
//      for. Nothing is swallowed: `fatal` is set and the diagnostics are
//      reported, so the caller still parses an Eof-only stream, still surfaces
//      the error, and still exits non-zero. What changes is that it does so
//      instead of reading past the end of an empty vector.
//
//  (2) the run produced tokens but LOST the terminator. NO legitimate path can
//      do that — the run appends the Eof itself, unconditionally, right before
//      its normal return — so this is an internal invariant break and it fails
//      LOUD. Silently appending an Eof here would paper over a real truncation
//      bug behind a stream the parser happily accepts, which is the "traded a
//      crash for a silent skip" outcome the fail-loud rule forbids.
//
// Everything it touches is DATA-DRIVEN: it reads the token vector's shape and
// the main buffer's name/id. It knows no language, no architecture and no
// object format.
void establishResultContract(PreprocessResult& result,
                             std::shared_ptr<SourceBuffer> const& mainSourcePtr,
                             DiagnosticBudget                     budget) {
    SourceBuffer const& mainSource = *mainSourcePtr;
    // Every path reports through this; an abort before the run allocated one
    // would otherwise hand the caller a null `unique_ptr` it dereferences.
    if (!result.diagnostics) {
        result.diagnostics = std::make_unique<DiagnosticReporter>(budget.asConfig());
    }
    result.mainSourceId = mainSource.id();
    // The MAIN source is an origin buffer of every preprocess result, by
    // definition — it is the input. The run derives `originBuffers` from the
    // LINE MAP, which an abort leaves empty, so on an aborting path the main
    // buffer was never collected and the diagnostics stamped with its id had
    // nothing to resolve against: MEASURED TF-C115, the collision error
    // rendered `--> <unknown-buffer:1>:offset 0` instead of naming the file the
    // user compiled. Stating the invariant here (deduped by id, exactly as the
    // run's own collection loop does) makes it hold on EVERY path rather than
    // on the paths that happen to build a line map.
    {
        bool present = false;
        for (auto const& ob : result.originBuffers) {
            if (ob && ob->id() == mainSource.id()) { present = true; break; }
        }
        if (!present) result.originBuffers.push_back(mainSourcePtr);
    }
    if (!result.synthBuffer) {
        // An EMPTY buffer under the MAIN FILE's name: every consumer that
        // dereferences `synthBuffer` (the Parser's source, the CU sidecar's
        // `source`, `makeRemap`'s synth id) gets a live object, and a span into
        // it is attributed to the file the user actually named rather than to
        // `<unknown-buffer>`.
        result.synthBuffer =
            SourceBuffer::fromString(std::string{}, std::string{mainSource.name()});
    }
    if (result.tokens.empty()) {
        Token eof;
        eof.coreKind = CoreTokenKind::Eof;
        eof.span     = SourceSpan::empty(
            static_cast<ByteOffset>(result.synthBuffer->size()));
        result.tokens.push_back(eof);
        return;
    }
    if (result.tokens.back().coreKind != CoreTokenKind::Eof) {
        ppFatal("preprocess: the result token vector is NOT Eof-terminated - a "
                "return path in preprocessRun produced tokens but dropped the "
                "terminating Eof, breaking the PreprocessResult contract every "
                "consumer reads (see establishResultContract)");
    }
}

} // namespace

PreprocessResult preprocess(
    std::shared_ptr<SourceBuffer>        mainSource,
    std::shared_ptr<GrammarSchema const> schema,
    std::span<fs::path const>            includeDirs,
    HeaderNameMatching                   headerNameMatching,
    DiagnosticBudget                     budget,
    std::span<fs::path const>            systemDirs,
    std::optional<ObjectFormatKind>      activeFormat,
    std::span<std::string const>         userDefines,
    std::span<PredefinedMacroDef const>  targetPredefinedMacros,
    std::span<PredefinedMacroDef const>  formatPredefinedMacros) {
    if (!mainSource || !schema) ppFatal("preprocess: null source or schema");
    if (!schema->preprocess().enabled) {
        ppFatal("preprocess: called with a schema whose preprocess pass is "
                "disabled - caller must gate on preprocess().enabled");
    }
    PreprocessResult result = preprocessRun(
        mainSource, std::move(schema), includeDirs, headerNameMatching,
        budget, systemDirs, activeFormat, userDefines, targetPredefinedMacros,
        formatPredefinedMacros);
    // ★ THE SINGLE EXIT. Its value is that it is not optional: a `return` added
    // anywhere inside `preprocessRun` — for a config fault nobody has thought
    // of yet — cannot bypass it.
    establishResultContract(result, mainSource, budget);
    return result;
}

} // namespace dss
