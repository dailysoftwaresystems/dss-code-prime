#include "analysis/preprocess/preprocessor.hpp"

#include "analysis/preprocess/pp_if_eval.hpp"
#include "core/types/include_path_resolve.hpp"
#include "core/substrate/phase_timers.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "tokenizer/tokenizer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
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

void emitPP(DiagnosticReporter& rep, DiagnosticCode code, BufferId buffer,
            SourceSpan span, std::string actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
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

std::vector<PPToken> tokenizeToPP(
    std::shared_ptr<SourceBuffer> const& buffer,
    std::shared_ptr<GrammarSchema const> const& schema,
    DiagnosticReporter& rep) {
    Tokenizer tk{buffer, schema};
    auto result = std::move(tk).tokenize();
    if (result.diagnostics) {
        for (auto const& d : result.diagnostics->all()) rep.report(d);
    }
    std::vector<PPToken> out;
    while (!result.stream.isAtEnd()) {
        Token t = result.stream.advance();
        if (t.coreKind == CoreTokenKind::Eof) break;
        out.push_back(PPToken{t, buffer->slice(t.span)});
    }
    return out;
}

} // namespace

LineMap::Resolved LineMap::resolve(ByteOffset synthOffset) const noexcept {
    if (segments_.empty()) return {};
    LineMapSegment const* best = &segments_.front();
    for (auto const& seg : segments_) {
        if (seg.synthStart <= synthOffset) best = &seg;
        else break;
    }
    Resolved r;
    r.origin = best->origin.get();
    const ByteOffset delta = (synthOffset >= best->synthStart)
                                 ? (synthOffset - best->synthStart) : 0;
    r.offset = best->originStart + delta;
    return r;
}

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
        if (!(buffer == synthId)) return;
        auto s = mapCopy.resolve(span.start());
        auto e = mapCopy.resolve(span.end());
        if (s.origin == nullptr) return;   // empty map -> leave on synth.
        buffer = s.origin->id();
        if (e.origin == s.origin && e.offset >= s.offset) {
            span = SourceSpan::of(s.offset, e.offset);
        } else {
            span = SourceSpan::empty(s.offset);
        }
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

// C21 (D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE / FINDING-B): the SINGLE
// per-object-format availability predicate for a config PREDEFINED macro. A
// macro with a non-empty `availableObjectFormats` set is available ONLY when the
// active object format is in that set; an EMPTY set means EVERY format; absent an
// active format only a universal (empty-set) macro is available. Shared by the
// include-gating pre-scan's definedness oracle (`sbNameDefined`), the value-seed
// prefix builder (`preprocess`), the `MacroExpander` `predefined_` seed, AND the
// function-like "<built-in>" prologue filter -- so all four apply ONE filter and
// can NEVER drift. A divergence between the pre-scan's seed and the authoritative
// `predefined_` set would be a silent P0016 seam (the pre-scan resolving a
// value-gated include the authoritative pass reads dead), so this MUST stay the
// one definition.
[[nodiscard]] bool predefinedMacroAvailableOnActiveFormat(
    std::vector<std::string> const&        availableObjectFormats,
    std::optional<ObjectFormatKind> const& activeFormat) {
    if (availableObjectFormats.empty()) return true;
    if (!activeFormat.has_value()) return false;
    return ffi::objectFormatInAvailabilitySet(availableObjectFormats,
                                              *activeFormat);
}

// Recursive synth-text builder. Tokenizes a file to FIND quote includes,
// splices the recursively-preprocessed header text in place of each quote
// include directive, and copies everything else (including angle includes)
// VERBATIM with a 1:1 line-map segment.
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
    DiagnosticReporter&                  rep;
    int                                  depth;
    std::vector<fs::path>&               includeStack;
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
    struct SbMacro {
        bool               functionLike = false;
        std::vector<Token> replacement;  // object-like body (spans in scanBuf)
    };
    std::unordered_map<std::string, SbMacro> localMacros;

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
        // `#ifdef NAME`. The predefined filter is the SHARED helper (FINDING-B) so
        // it can never drift from the value prefix's predefined subset.
        if (localMacros.find(std::string{n}) != localMacros.end()) return true;
        for (PredefinedMacroDef const& pm : schema->preprocess().predefinedMacros) {
            if (pm.name != n) continue;
            return predefinedMacroAvailableOnActiveFormat(
                pm.availableObjectFormats, activeFormat);
        }
        return false;
    }

    std::optional<fs::path> resolveQuote(std::string_view filename,
                                         fs::path const& includingDir) const {
        // An empty include target names nothing -- fail loud (the caller
        // emits P_PreprocessorIncludeError). Only a REGULAR file is a valid
        // include target; `is_regular_file` excludes a directory (so
        // `#include ""` cannot resolve to the including dir itself, which
        // would later throw in SourceBuffer::fromFile).
        if (filename.empty()) return std::nullopt;
        fs::path const rel{filename};
        std::error_code ec;
        if (rel.is_absolute()) {
            return fs::is_regular_file(rel, ec) ? std::optional<fs::path>{rel}
                                                : std::nullopt;
        }
        if (!includingDir.empty()) {
            if (auto c = includingDir / rel; fs::is_regular_file(c, ec)) return c;
        }
        for (fs::path const& dir : includeDirs) {
            if (auto c = dir / rel; fs::is_regular_file(c, ec)) return c;
        }
        return std::nullopt;
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
                                                   bool reportMalformed) {
        if (systemDirs.empty()) return SystemMacroSplice::NotAvailable;
        auto descPath = resolveSystemDescriptor(headerName, systemDirs);
        if (!descPath) return SystemMacroSplice::NotAvailable;
        // If the PARENT descriptor declares this header unavailable on the active
        // object-format, treat it EXACTLY like "no descriptor on the path" — the
        // semantic gate then fails loud + `__has_include` returns false, so all
        // three descriptor consumers stay consistent (c9 MUST-FIX-3). This drives
        // the NotAvailable return (the caller leaves the include verbatim).
        if (activeFormat.has_value()
            && !ffi::shippedHeaderAvailableForFormat(*descPath, *activeFormat)) {
            return SystemMacroSplice::NotAvailable;
        }
        // Reconstruct one neutral macro as a `#define` line into `out`; the
        // downstream tokenizer + handleDefine build the MacroDef with the proven
        // function-like / param / redefinition machinery (an identical re-define on
        // a double-include is idempotent).
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
        std::unordered_set<std::string> visited;   // per-call (a splice is one root)
        ffi::forEachDescriptorInClosure(
            *descPath, systemDirs, visited,
            [&](fs::path const& p) {
                bool const isParent = !sawParent;
                sawParent = true;
                // Per-descriptor availability (the parent already passed; a sibling
                // absent on this format contributes no macros — mirrors today's
                // single-descriptor behavior applied to each closure member).
                if (activeFormat.has_value()
                    && !ffi::shippedHeaderAvailableForFormat(p, *activeFormat)) {
                    return;
                }
                DiagnosticReporter macroRep;   // throwaway — malformed surfaced downstream
                // Pass the active object-format so a per-FORMAT macro variant selects
                // the right replacement; nullopt ⇒ a variants-only macro is not injected.
                auto macros = ffi::readShippedLibMacros(p, macroRep, activeFormat);
                if (!macros) { if (isParent) parentMacrosMalformed = true; return; }
                for (auto const& macro : *macros) spliceMacro(macro);
            },
            [&](std::string const&) { /* import resolver owns F_ShippedHeaderNotFound */ });

        if (parentMacrosMalformed) {
            // Malformed PARENT descriptor: report ONLY on a confidently-live include
            // (the `reportMalformed` note above). A dead/uncertain branch stays
            // silent — dead-branch inertness — while the include is left verbatim by
            // the caller (Malformed != Spliced) and elided by the authoritative pass.
            if (reportMalformed) {
                emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError, BufferId{},
                       SourceSpan::empty(0),
                       std::string{"shipped-header descriptor malformed (macros): "}
                           + descPath->generic_string());
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
    void sbTrackDefine(std::vector<PPToken> const& toks, std::size_t nameP,
                       std::size_t end) {
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
            for (std::size_t q = p; q < end; ++q) {
                if (isNewline(toks[q].tok)) break;
                if (isTrivia(toks[q].tok)) continue;
                m.replacement.push_back(toks[q].tok);
            }
        }
        localMacros[name] = std::move(m);
    }

    // c17: object-like macro expansion over `localMacros` for the
    // `sbEvalIfOperand` `#if` evaluation. A bounded recursive rescan (so a
    // `#define A B` / `#define B 1` chain folds, the common object-like case),
    // with an `active`-set self-reference guard (a `#define X X` freezes to its
    // own name, matching the full engine) + a depth backstop. FUNCTION-like
    // names are NEVER expanded here -- an invocation is already detected as
    // conservative by `sbEvalIfOperand`. Replacement tokens slice against `buf`
    // (the scan buffer the `#define` came from), so their spans stay valid for
    // the ICE parser.
    std::vector<Token> sbExpand(std::vector<Token> const& in,
                                SourceBuffer const& buf,
                                std::set<std::string>& active, int depth) const {
        if (depth > 32) return in;   // backstop (a pathological cycle the guard
                                     // missed never loops the host)
        std::vector<Token> outToks;
        outToks.reserve(in.size());
        for (Token const& t : in) {
            if (t.coreKind == CoreTokenKind::Word) {
                std::string name{buf.slice(t.span)};
                auto it = localMacros.find(name);
                if (it != localMacros.end() && !it->second.functionLike
                    && active.find(name) == active.end()) {
                    active.insert(name);
                    std::vector<Token> sub =
                        sbExpand(it->second.replacement, buf, active, depth + 1);
                    active.erase(name);
                    for (Token const& s : sub) outToks.push_back(s);
                    continue;
                }
            }
            outToks.push_back(t);
        }
        return outToks;
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

        PpMacroExpand expandCb =
            [this, &buf](std::vector<Token> const& in) {
                std::set<std::string> active;
                return sbExpand(in, buf, active, 0);
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
                auto descPath = resolveSystemDescriptor(filename, systemDirs);
                if (!descPath) return false;
                if (activeFormat.has_value()
                    && !ffi::shippedHeaderAvailableForFormat(*descPath,
                                                             *activeFormat)) {
                    return false;
                }
                return true;
            }
            return resolveQuote(filename, includingDir).has_value();
        };
        PpProductText productCb = []() { return std::string_view{}; };
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
            auto resolved = resolveQuote(filename, includingDir);
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
        if (!v.has_value()) {
            uncertain = true;   // malformed/unsupported -> conservative (skip)
            return false;
        }
        return *v != 0;
    }

    void build(std::shared_ptr<SourceBuffer> const& source,
               std::string& out, LineMap& map) {
        const char dquote  = '"';
        const char newline = '\n';
        if (depth > kMaxIncludeDepth) {
            emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError, BufferId{},
                   SourceSpan::empty(0),
                   std::string{"include nesting too deep (possible cycle): "}
                       + std::string{source->name()});
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
        std::string spliced = preScanDefinePrefix;
        std::size_t const prefixLen = spliced.size();
        LineMap     localMap;
        appendWithContinuationSplice(source->text(), source, 0, spliced,
                                     localMap);

        auto scanBuf =
            SourceBuffer::fromString(spliced, std::string{source->name()});
        DiagnosticReporter scratch;
        auto toks = tokenizeToPP(scanBuf, schema, scratch);

        const auto hashKind =
            schema->schemaTokens().find(cfg().directiveIntroToken);
        const auto quoteKind =
            schema->schemaTokens().find(cfg().quoteIncludeToken);
        const auto angleKind =
            schema->schemaTokens().find(cfg().angleIncludeToken);

        // C21: start PAST the non-emitted value prefix (load-bearing invariant 2).
        std::size_t copiedUpTo = prefixLen;
        fs::path const includingDir = fs::path{source->name()}.parent_path();

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

        for (std::size_t i = 0; i < toks.size(); ++i) {
            if (!isHash(toks[i].tok)) continue;
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
            if (sbStackActive(sbCondStack) && dirWord == cfg().defineDirective) {
                std::size_t const lineEndTok = sbLineEndTok(i);
                sbTrackDefine(toks, j + 1, lineEndTok);
                i = lineEndTok - 1;
                continue;
            }
            if (sbStackActive(sbCondStack) && dirWord == cfg().undefDirective) {
                std::size_t const lineEndTok = sbLineEndTok(i);
                std::size_t u = j + 1;
                while (u < lineEndTok && isTrivia(toks[u].tok)) ++u;
                if (u < lineEndTok && toks[u].tok.coreKind == CoreTokenKind::Word) {
                    localMacros.erase(std::string{toks[u].text});
                }
                i = lineEndTok - 1;
                continue;
            }

            if (dirWord != cfg().includeDirective) continue;
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
                // Splice the descriptor's macros into a LOCAL buffer FIRST (so a
                // NotAvailable outcome touches neither `out` nor the line-map). On
                // NotAvailable (no descriptor / unavailable) OR Malformed (already
                // emitted) leave the include fully verbatim. Otherwise copy up to
                // the directive, emit the `#define` lines, then KEEP the include
                // line in place (copiedUpTo = dStart) so the post-parse import
                // resolver still injects the typed surfaces (a typed-only
                // descriptor splices zero macros but the line is still kept).
                std::string defs;
                // Splice UNGATED (the authoritative pass arbitrates liveness), but
                // report a malformed descriptor ONLY on a confidently-live include
                // (`includeResolvable()`) — restoring the pre-change dead-branch
                // inertness of that diagnostic.
                if (spliceSystemDescriptorMacros(angleName, defs,
                                                 /*reportMalformed=*/includeResolvable())
                    != SystemMacroSplice::Spliced) {
                    continue;
                }
                const ByteOffset dStart = toks[i].tok.span.start();
                copyVerbatim(spliced, localMap, copiedUpTo, dStart, out, map);
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
                dirEnd   = toks[bodyIdx].tok.span.end();
            }
            // Consume the closing quote char if it physically follows.
            if (dirEnd < spliced.size() && spliced[dirEnd] == dquote) ++dirEnd;

            auto resolved = resolveQuote(filename, includingDir);
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
                    // This fallback is reached only PAST the quote arm's
                    // `includeResolvable()` gate (below), so the include is
                    // confidently-live here — report a malformed descriptor loud.
                    SystemMacroSplice const sr =
                        spliceSystemDescriptorMacros(filename, defs,
                                                     /*reportMalformed=*/includeResolvable());
                    if (sr != SystemMacroSplice::NotAvailable) {
                        // Malformed already emitted its own error; on Spliced the
                        // macros are in `defs`. Either way rewrite quote→angle:
                        // emit the descriptor macros, then a synthetic
                        // `#include <filename>` in place of the quote bytes.
                        copyVerbatim(spliced, localMap, copiedUpTo, dirStart,
                                     out, map);
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
                continue;
            }
            std::error_code ec;
            fs::path canon = fs::weakly_canonical(*resolved, ec);
            if (ec) canon = *resolved;
            if (std::find(includeStack.begin(), includeStack.end(), canon)
                != includeStack.end()) {
                emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError,
                       BufferId{}, SourceSpan::empty(0),
                       std::string{"circular include of "} + filename);
                continue;
            }
            auto headerBuf = SourceBuffer::fromFile(*resolved);
            if (!headerBuf) {
                emitPP(rep, DiagnosticCode::P_PreprocessorIncludeError,
                       BufferId{}, SourceSpan::empty(0),
                       std::string{"quote include unreadable: "} + filename);
                continue;
            }

            copyVerbatim(spliced, localMap, copiedUpTo, dirStart, out, map);

            includeStack.push_back(canon);
            SynthBuilder child{schema, includeDirs, systemDirs, activeFormat, rep,
                               depth + 1, includeStack, fatal, preScanDefinePrefix};
            child.build(headerBuf, out, map);
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
};

// Lift a plain (directive-stripped, original) body token into the expansion
// working set: empty hide set + its OWN synth offset as the invocation anchor
// (FC15b). Every original source token thus seeds `__LINE__`/`__FILE__` against
// its real position; macro splices later inherit the invoking token's anchor.
inline ExpToken fromToken(Token const& t) {
    return ExpToken{t, nullptr, t.span.start()};
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
};

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
                  LineMap const* lineMap,
                  std::span<fs::path const> includeDirs = {},
                  std::span<fs::path const> systemDirs = {},
                  std::optional<ObjectFormatKind> activeFormat = {},
                  fs::path includingDir = {})
        : synth_(std::move(synth)), schema_(std::move(schema)), rep_(rep),
          prefixLen_(prefixLen), lineMap_(lineMap),
          includeDirs_(includeDirs), systemDirs_(systemDirs),
          activeFormat_(activeFormat),
          includingDir_(std::move(includingDir)) {
        // FC15b (predefined macros; C 6.10.8): seed the predefined-macro map
        // (name -> def) from config. An identifier that is NOT a `#define`d
        // macro but IS a predefined name materializes its configured value (see
        // `expand`). EMPTY when the language declares none (toy / tsql), so the
        // engine is a strict identity pass for `__LINE__` &c.
        for (PredefinedMacroDef const& pm : cfg().predefinedMacros) {
            // Per-format availability filter (mirrors the shipped-header gate):
            // a macro with a non-empty `availableObjectFormats` is seeded ONLY
            // when the active object format is in its set. EMPTY ⇒ every format.
            // A nullopt activeFormat_ (no target selected) seeds the macro
            // UNCONDITIONALLY only when the filter is empty; a format-restricted
            // macro stays unseeded absent a format (it is meaningless without
            // one). This lets `_WIN32` be predefined for the pe target ONLY.
            // FINDING-B (C21): the SHARED per-format filter, so this authoritative
            // `predefined_` seed can never drift from the pre-scan's value prefix +
            // sbNameDefined.
            if (!predefinedMacroAvailableOnActiveFormat(pm.availableObjectFormats,
                                                        activeFormat_)) {
                continue;
            }
            // c105 (D-PP-FUNCTION-LIKE-PREDEFINE): a FUNCTION-LIKE predefine is
            // NOT seeded here — it lowers to a `#define name(params) value`
            // line in the "<built-in>" prologue (see `preprocess()`), making it
            // an ORDINARY macro (the directive handler owns its param parsing +
            // expansion). Seeding it here too would make the prologue #define
            // trip the C 6.10.8.1 predefined-collision guard against itself.
            if (pm.isFunctionLike) continue;
            predefined_.emplace(pm.name, pm);
        }
        // FC15b: compute the translation DATE/TIME spellings ONCE (C 6.10.8.1 --
        // both stay CONSTANT through a translation unit). `__DATE__` is
        // `"Mmm dd yyyy"` with a SPACE-padded day (e.g. `"Jun  4 2026"`);
        // `__TIME__` is `"hh:mm:ss"`. Computed only when at least one date/time
        // macro is declared (no `std::time` call for a language that needs none).
        bool needDate = false, needTime = false;
        for (PredefinedMacroDef const& pm : cfg().predefinedMacros) {
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
                if (isMutatingDirective(in, i)) {
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
        std::vector<ExpToken> work;
        work.reserve(pending.size());
        for (Token const& t : pending) work.push_back(fromToken(t));
        std::vector<ExpToken> expanded = expand(std::move(work), 0);
        // FC15 paste residuals: a placemarker is normally consumed inside
        // `substitute`; this BACKSTOP drops any stray one so it never reaches the
        // parser as a garbage (default-constructed) token.
        for (ExpToken const& et : expanded) {
            if (et.placemarker) continue;
            accumulated.push_back(et.tok);
        }
    }

    // c18: TRUE iff the directive line at `hashIdx` is a table-MUTATING
    // `#define`/`#undef` while the conditional stack is active (so it would
    // actually be processed -- a dead-branch define is gated out in
    // `handleDirective`). Pure: mirrors `handleDirective`'s own skipTrivia +
    // word-read (so it sees the SAME directive word) without mutating any state.
    // `run()` consults it to flush the pending body BEFORE the table changes.
    [[nodiscard]] bool isMutatingDirective(std::vector<Token> const& in,
                                           std::size_t hashIdx) const {
        if (!stackActive()) return false;
        const std::size_t end = lineEnd(in, hashIdx);
        const std::size_t p   = skipTrivia(in, hashIdx + 1);
        if (p >= end || isNewline(in[p])) return false;
        const std::string_view w = text(in[p]);
        return w == cfg().defineDirective || w == cfg().undefDirective;
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

        // FC15c (`#pragma`; C 6.10.6): consume-and-DROP the whole line with NO
        // error. C 6.10.6p2 lets an implementation ignore a pragma it does not
        // recognize; DSS recognizes none, so every `#pragma` is silently dropped
        // (the line's tokens are NOT emitted into `body`). Placed AFTER the
        // dead-branch gate (so a `#pragma` in an elided branch is silent too) and
        // BEFORE the generic unsupported-directive `else`. Config-matched
        // (`pragmaDirective`), never a hard-coded "pragma"; an empty config field
        // (a language without `#pragma`) skips this arm -> the line falls through
        // to the generic unsupported-directive fail-loud.
        if (!cfg().pragmaDirective.empty() && word == cfg().pragmaDirective) {
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
            // for a quote form (now CONDITIONAL-aware: a DEAD-branch quote
            // include is left verbatim + reaches HERE only if live) and passed
            // through for an angle form (to the post-parse import resolver). The
            // line's text is forwarded so the resolver still sees the angle form
            // (its tokens are inert to the parser). The former ordering hazard
            // (a dead-branch quote include resolved upstream) no longer exists:
            // SynthBuilder gates quote resolution on its own conditional scan.
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
        return table_.find(std::string{name}) != table_.end()
            || predefined_.find(std::string{name}) != predefined_.end();
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
        // FC15c: `__has_include` resolves a header EXACTLY as the include
        // machinery would (Finding 3): quote form = self-dir + includeDirs
        // (`resolveIncludePath`); angle form = `<stem>.json` on systemDirs
        // (`resolveSystemDescriptor` -- the SHARED mapping the import resolver
        // also calls, so the two never disagree).
        PpHasInclude hasIncludeCb =
            [this](std::string_view filename, bool isAngle) -> bool {
            if (isAngle) {
                auto descPath = resolveSystemDescriptor(filename, systemDirs_);
                if (!descPath) return false;  // no descriptor on the path
                // c9 (Phase-2): per-target truth — when the active object-format is
                // known, a header whose descriptor excludes it reports NOT available
                // (agreeing with the `#include` semantic gate + the macro-splice).
                // nullopt activeFormat_ = pure existence (unchanged pre-c9 behavior).
                if (activeFormat_.has_value()
                    && !ffi::shippedHeaderAvailableForFormat(*descPath,
                                                             *activeFormat_)) {
                    return false;
                }
                return true;
            }
            return resolveIncludePath(filename, includingDir_, includeDirs_)
                .has_value();
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
            auto resolved = resolveIncludePath(filename,
                                               embedResolutionDir(opSpan),
                                               includeDirs_);
            if (!resolved) return 0;                            // NOT_FOUND
            std::error_code ec;
            if (!fs::is_regular_file(*resolved, ec)) return 0;  // NOT_FOUND
            auto const sz = fs::file_size(*resolved, ec);
            if (ec) return 0;                                   // stat failed
            return sz == 0 ? 2 /*EMPTY*/ : 1 /*FOUND*/;
        };
        auto v = evaluateIfExpression(operand, *schema_, expandCb, definedCb,
                                      hasIncludeCb, *synth_, productCb, rep_,
                                      embedCb);
        return v.has_value() && *v != 0;
    }

    // Macro-expand a token run with the SAME engine `run()` uses (object +
    // function-like, hide-set-precise): lift into the ExpToken working set,
    // expand, drop the hide sets. Used by the `#if` evaluator's callback so the
    // controlling expression's macros expand identically to the body's.
    std::vector<Token> expandTokens(std::vector<Token> const& toks) {
        std::vector<ExpToken> work;
        work.reserve(toks.size());
        // FC15b: seed each token's own offset as its invocation anchor (a
        // `__LINE__` in a `#if` operand resolves against that operand's line).
        for (Token const& t : toks) work.push_back(fromToken(t));
        std::vector<ExpToken> expanded = expand(std::move(work), 0);
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
        // The shared `resolveIncludePath` matches on `fs::exists`, so it can hand
        // back a DIRECTORY. Require a regular file (nullopt -> the caller's loud
        // unreadable diagnostic) so a directory-named resource fails LOUD, never
        // reads as a silently-empty embed.
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return std::nullopt;
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        std::ostringstream buf;
        buf << in.rdbuf();
        // A mid-stream IO error (disk/share failure) can silently truncate the
        // read; check the stream state so a truncated resource is a LOUD error,
        // never a quietly-shortened embed.
        if (in.bad()) return std::nullopt;
        return std::move(buf).str();
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
                return fs::path{std::string{r.origin->name()}}.parent_path();
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
        // the filename empty -> loud below. The closing `"` byte is absorbed into
        // the StringLiteral (coalesce:true) and produces NO token (the working
        // `__has_include("h")` / SynthBuilder precedent), so it can't be mistaken
        // for a trailing parameter.
        std::string filename;
        std::size_t after = p + 1;   // token index just past the filename body
        if (after < end && !isTrivia(in[after]) && !isNewline(in[after])
            && in[after].span.start() == in[p].span.end()) {
            filename = std::string{text(in[after])};
            ++after;
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
        auto resolved = resolveIncludePath(filename, embedResolutionDir(dirSpan),
                                           includeDirs_);
        if (!resolved) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorEmbed, synth_->id(),
                   dirSpan, std::string{"#embed resource not found: "} + filename);
            return;
        }

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

        // FC15b (C 6.10.8.1p2): a PREDEFINED macro name shall not be the subject
        // of a `#define`. Reject loudly and DO NOT alter the table (the predefined
        // name keeps materializing its configured value). Looked up in the
        // config-seeded set, so the engine never hard-codes a name.
        if (predefined_.find(name) != predefined_.end()) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPredefinedMacro,
                   synth_->id(), in[nameIdx].span,
                   std::string{"'"} + name
                       + "' is a predefined macro and may not be #defined");
            return;
        }

        MacroDef def;
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
        for (std::size_t q = skipTrivia(in, p); q < end;
             q = skipTrivia(in, q + 1)) {
            if (isNewline(in[q])) break;
            def.replacement.push_back(in[q]);
            if (!repText.empty()) repText.push_back(space);
            repText.append(text(in[q]));
        }
        def.text = std::move(repText);

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

        auto it = table_.find(name);
        if (it != table_.end() && !sameDefinition(it->second, def)) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorMacroRedefinition,
                   synth_->id(), in[nameIdx].span,
                   std::string{"incompatible redefinition of macro: "} + name);
            return;
        }
        table_[name] = std::move(def);
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
            && a.text == b.text;
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
        // FC15b (C 6.10.8.1p2): a PREDEFINED macro name shall not be the subject
        // of a `#undef`. Reject loudly and DO NOT touch the table (config-seeded
        // lookup, no hard-coded name).
        if (predefined_.find(name) != predefined_.end()) {
            emitPP(rep_, DiagnosticCode::P_PreprocessorPredefinedMacro,
                   synth_->id(), in[p].span,
                   std::string{"'"} + name
                       + "' is a predefined macro and may not be #undef'd");
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

    // Build the substituted replacement list for a function-like call (C 6.10.3).
    // A replacement token that names a parameter (or `__VA_ARGS__`) is replaced by
    // that argument's tokens; every other token passes through stamped with `hs`.
    // FC15a adds the `#` (stringize, C 6.10.3.2) and `##` (token-paste, C 6.10.3.3)
    // operators in TWO phases:
    //   PHASE A -- substitution. A normal parameter substitutes its PRE-EXPANDED
    //   argument (`expandedArgs[k]` / `vaArgs`, C 6.10.3.1). A `#` immediately
    //   followed by a parameter is replaced by ONE string-literal product
    //   (`stringizeArg`, F2) built from that parameter's RAW argument
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
        // FC15b: a REPLACEMENT-origin token (a plain replacement token, a `##`
        // marker/product, a stringize product) inherits the INVOCATION offset
        // `invOffset` (so a `__LINE__` in the replacement resolves to the
        // invocation line). An ARGUMENT token keeps its OWN `invOffset` (it came
        // from the call site -- its real position).
        auto stampArg = [&](std::vector<ExpToken> const& a,
                            std::vector<ExpToken>& outTokens) {
            for (ExpToken const& e : a) {
                outTokens.push_back(
                    ExpToken{e.tok, hideUnionAll(e.hide, hs), e.invOffset});
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

        // ── PHASE A: substitution (keeping `##` markers verbatim). ──
        std::vector<ExpToken> items;
        const std::size_t n = def.replacement.size();
        for (std::size_t i = 0; i < n; ++i) {
            Token const& r = def.replacement[i];
            if (isStringize(r)) {
                // `#` operand is the NEXT significant token, which MUST be a
                // parameter (or `__VA_ARGS__`).
                if (i + 1 < n) {
                    if (auto const* raw = rawArgAt(i + 1)) {
                        stringizeArg(*raw, hs, invOffset, items);
                        ++i;   // consume the parameter operand
                        continue;
                    }
                }
                emitPP(rep_, DiagnosticCode::P_PreprocessorStringize, synth_->id(),
                       r.span,
                       "'#' in a macro replacement must be followed by a "
                       "parameter");
                items.push_back(ExpToken{r, hs, invOffset});  // recovery: `#` verbatim
                continue;
            }
            if (isPaste(r)) {
                items.push_back(ExpToken{r, hs, invOffset});  // marker for phase B
                continue;
            }
            if (isVaArgsName(r, def)) {
                // RAW iff this `__VA_ARGS__` is a `##` operand (adjacent `##`).
                bool const pasteOperand =
                    (i > 0 && isPaste(def.replacement[i - 1]))
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
                    && i > 0 && isPaste(def.replacement[i - 1])
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
                continue;
            }
            int const pi = paramIndexOf(r, def);
            if (pi >= 0) {
                bool const pasteOperand =
                    (i > 0 && isPaste(def.replacement[i - 1]))
                    || (i + 1 < n && isPaste(def.replacement[i + 1]));
                // FC15 paste residuals: a `##`-operand parameter with an EMPTY
                // argument becomes a PLACEMARKER (C 6.10.3.3p2) via stampArgOrPM;
                // a non-operand parameter keeps the byte-identical pre-expanded path.
                if (pasteOperand) {
                    stampArgOrPM(rawArgs[static_cast<std::size_t>(pi)], items);
                } else {
                    stampArg(expandedArgs[static_cast<std::size_t>(pi)], items);
                }
                continue;
            }
            // A plain replacement token gets EXACTLY hs (no prior hide set) and
            // the INVOCATION offset (FC15b: a `__LINE__` in a function-like
            // replacement resolves to the invocation line).
            items.push_back(ExpToken{r, hs, invOffset});
        }

        // ── PHASE B: collapse every `##` marker LEFT-TO-RIGHT. ──
        return collapsePastes(std::move(items), hs, invOffset);
    }

    // Phase B of `substitute`: walk `items`, and at each `##` MARKER concatenate
    // the token immediately before it with the one immediately after into a
    // single re-tokenized product (F1), splicing `[i-1, i, i+1)` -> product and
    // RESCANNING from there (so `a##b##c` collapses left-to-right to one token).
    // A `##` at the very start or end of the list (no operand on one side) is a
    // constraint violation (C 6.10.3.3p1) -> P_PreprocessorPaste, recovery: drop
    // the dangling `##` and keep the lone operand. A product that is not exactly
    // one token (F1) -> P_PreprocessorPaste, recovery: emit both operands verbatim.
    std::vector<ExpToken> collapsePastes(std::vector<ExpToken> items,
                                         HideSet const& hs,
                                         ByteOffset invOffset) {
        std::size_t i = 0;
        while (i < items.size()) {
            if (!isPaste(items[i].tok)) { ++i; continue; }
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
            const std::size_t lo = i - 1;
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(lo),
                        items.begin() + static_cast<std::ptrdiff_t>(i + 2));
            items.insert(items.begin() + static_cast<std::ptrdiff_t>(lo),
                         ExpToken{*product, hs, invOffset});
            i = lo;   // rescan from the product
        }
        // FC15 paste residuals (MUST-FIX-2): drop any PLACEMARKER that survived
        // collapse (e.g. `J(,)` -> a lone placemarker, or a placemarker operand that
        // never met a `##`). Every `##` marker is consumed within this single call,
        // so a surviving placemarker is dead -- removing it HERE guarantees a
        // placemarker never re-enters `expand`'s rescan (the `run()`/`expandTokens()`
        // drop is a defensive backstop only).
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [](ExpToken const& e) { return e.placemarker; }),
                    items.end());
        return items;
    }

    // FC15a (F2, C 6.10.3.2): STRINGIZE the RAW argument token run `raw` into a
    // string-literal product, appending the resulting token(s) to `out`. Per
    // C 6.10.3.2p2 the spelling is the argument's SOURCE text with: every run of
    // white space (incl. between tokens) collapsed to a single space and
    // leading/trailing space deleted; and a `\` inserted before each `"` and `\`
    // (the chars of a string/char literal -- in valid C those characters appear
    // ONLY inside such a literal, so escaping every occurrence is exact). The
    // result is wrapped in `"..."`, appended to `productText_` (A2), and
    // RE-TOKENIZED so the product is a real StringStart + StringLiteral pair
    // (a single fabricated token would not satisfy the grammar's
    // `stringLiteralExpr = StringStart StringLiteral`) whose spans point at the
    // appended region. Each product token is stamped with `hs`.
    void stringizeArg(std::vector<ExpToken> const& raw, HideSet const& hs,
                      ByteOffset invOffset, std::vector<ExpToken>& out) {
        std::string inner = "\"";
        if (!raw.empty()) {
            // The raw operand's tokens are un-pre-expanded args from the CALL
            // site, so they are contiguous in the prefix buffer: one slice
            // recovers the exact source spelling (incl. interior string quotes
            // and whitespace). The CLOSING delimiter of a string/char literal
            // that ENDS the argument was consumed by the tokenizer (no token
            // covers it), so extend the slice by one byte when it is a `"`/`'`.
            const ByteOffset s = raw.front().tok.span.start();
            ByteOffset e = raw.back().tok.span.end();
            if (e < prefixLen_) {
                const std::string_view tail = synth_->slice(e, e + 1);
                if (tail.size() == 1 && (tail[0] == '"' || tail[0] == '\'')) ++e;
            }
            appendStringized(synth_->slice(s, e), inner);
        }
        inner.push_back('"');
        // FC15b: a stringize product is a replacement-origin token -> inherit
        // the invocation offset (kept consistent with the other product paths).
        for (Token const& t : materializeSignificant(inner)) {
            out.push_back(ExpToken{t, hs, invOffset});
        }
    }

    // Append `src` to `out` realizing C 6.10.3.2's stringize transform: collapse
    // each run of source white space to a single space and drop leading/trailing
    // space; insert a `\` before each `"` and `\`.
    static void appendStringized(std::string_view src, std::string& out) {
        std::size_t i = 0;
        const std::size_t nbytes = src.size();
        // Skip leading white space.
        auto isWs = [](char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r'
                || c == '\f' || c == '\v';
        };
        while (i < nbytes && isWs(src[i])) ++i;
        bool pendingSpace = false;
        for (; i < nbytes; ++i) {
            char const c = src[i];
            if (isWs(c)) { pendingSpace = true; continue; }
            if (pendingSpace) { out.push_back(' '); pendingSpace = false; }
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
    }

    // FC15b (predefined macros; C 6.10.8.1): compute the once-per-TU translation
    // DATE / TIME spellings from the wall clock. `__DATE__` is the C-mandated
    // `"Mmm dd yyyy"` with a SPACE-padded day of month (`"Jun  4 2026"`, two
    // leading spaces for a single-digit day); `__TIME__` is `"hh:mm:ss"`
    // (zero-padded). Stored WITHOUT the surrounding quotes (the materializer
    // wraps them). Uses `std::localtime` (the translation's local date, matching
    // a hosted C implementation). Defensive: a null `localtime` (impossible in
    // practice) leaves the strings empty -> a synth `""` literal, never a crash.
    void computeDateTime(bool needDate, bool needTime) {
        const std::time_t now = std::time(nullptr);
        const std::tm* lt = std::localtime(&now);
        if (lt == nullptr) return;
        if (needDate) {
            static char const* const kMon[12] = {
                "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            const int mon = (lt->tm_mon >= 0 && lt->tm_mon < 12) ? lt->tm_mon : 0;
            char buf[16];
            // SPACE-padded day (`%e` is not portable on MSVC), 4-digit year.
            std::snprintf(buf, sizeof(buf), "%s %2d %04d", kMon[mon],
                          lt->tm_mday, lt->tm_year + 1900);
            dateString_ = buf;
        }
        if (needTime) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", lt->tm_hour,
                          lt->tm_min, lt->tm_sec);
            timeString_ = buf;
        }
    }

    // FC15b (predefined macros; C 6.10.8.1): MATERIALIZE the replacement token(s)
    // of a predefined macro at an invocation whose anchor offset is `invOffset`.
    // The spelling is built then routed through `materializeSignificant` (the
    // FC15a A2 mechanism) so the value reaches the REAL parser via a real span in
    // the synth buffer -- a Number for `__LINE__`/Constant, a StringStart +
    // StringLiteral pair for `__FILE__`/`__DATE__`/`__TIME__` (exactly as a
    // stringize product). Dispatches ONLY on `def.kind`, never the name.
    std::vector<Token> materializePredefined(PredefinedMacroDef const& def,
                                             ByteOffset invOffset) {
        switch (def.kind) {
        case PredefinedMacroKind::Line: {
            // C 6.10.8.1: the LINE number of the macro's INVOCATION. Resolve the
            // invocation offset through the line-map to its ORIGIN buffer +
            // offset, then read the 1-based origin line. Null/empty line-map ->
            // line 1 (defensive; a real TU always has a map).
            std::uint32_t line = 1;
            if (lineMap_ != nullptr && !lineMap_->empty()) {
                LineMap::Resolved const r = lineMap_->resolve(invOffset);
                if (r.origin != nullptr) line = r.origin->lineCol(r.offset).line;
            }
            return materializeSignificant(std::to_string(line));
        }
        case PredefinedMacroKind::File: {
            // C 6.10.8.1: the presumed NAME of the current source file. Resolve
            // the invocation offset to its ORIGIN buffer so a `__FILE__` inside an
            // `#include`'d header reports the HEADER's name, not the main file's.
            // `\` -> `/` normalized, then quoted as a C string literal.
            std::string name = "<source>";   // defensive synth name
            if (lineMap_ != nullptr && !lineMap_->empty()) {
                LineMap::Resolved const r = lineMap_->resolve(invOffset);
                if (r.origin != nullptr) name = std::string{r.origin->name()};
            }
            for (char& c : name) {
                if (c == '\\') c = '/';
            }
            return materializeSignificant(quoteCString(name));
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
    std::vector<Token> materializeSignificant(std::string_view spelling) {
        const ByteOffset productBase =
            static_cast<ByteOffset>(productText_.size());
        productText_.append(spelling);
        auto tiny = SourceBuffer::fromString(std::string{spelling}, "<pp-product>");
        DiagnosticReporter scratch;
        auto ppToks = tokenizeToPP(tiny, schema_, scratch);
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
        while (!work.empty()) {
            // Audit fix #2 (UAF ordering): COPY the front token before any
            // pop/splice below -- `t.tok`, `t.hide`, `t.invOffset`, `t.tok.span`
            // and `name` must all stay valid across the pop_front/push_front that
            // `spliceOver` (or the emit-then-pop arms) perform on `work`. A
            // REFERENCE into `work.front()` would dangle the instant the front is
            // popped.
            ExpToken t = work.front();
            if (!isWord(t.tok)) {
                out.push_back(std::move(t));
                work.pop_front();
                continue;
            }
            const std::string name{text(t.tok)};
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
                        spliceOver(work, 0, 1, repl);
                        continue;   // rescan from the materialized value
                    }
                }
                out.push_back(std::move(t));
                work.pop_front();
                continue;
            }
            MacroDef const& def = it->second;
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
                for (Token const& r : def.replacement) {
                    repl.push_back(ExpToken{r, hs, t.invOffset});
                }
                // FC15 paste residuals (D-PP-PASTE-OBJECT-LIKE, C 6.10.3.3): `##`
                // applies to OBJECT-like macros too. Route the replacement through
                // the SAME `collapsePastes` chokepoint the function-like path uses
                // (no duplicated paste logic). An object-like macro has no parameters,
                // so no placemarker can arise here; `collapsePastes` collapses the
                // literal `##` operators and still fail-louds a genuine dangling `##`
                // (`#define OBJ a ##`). `#` (stringize) does NOT apply to object-like
                // macros (C 6.10.3.2) and there is none to handle here.
                repl = collapsePastes(std::move(repl), hs, t.invOffset);
                spliceOver(work, 0, 1, repl);
                continue;          // rescan from i (the first replacement token)
            }
            // FUNCTION-like: an invocation ONLY if the next significant token is
            // the configured `(`. Otherwise emit the name VERBATIM (C 6.10.3p10:
            // a function-like name not followed by `(` is not an invocation).
            std::size_t openIdx = nextSignificant(work, 1);
            if (openIdx >= work.size() || !isParenOpen(work[openIdx].tok)) {
                out.push_back(std::move(t));
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
                out.push_back(std::move(t));
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
                out.push_back(std::move(t));
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
    std::unordered_map<std::string, MacroDef> table_;
    // FC15b (predefined macros; C 6.10.8): the config-seeded predefined-macro
    // set (name -> def), the synth-offset -> origin line-map (for the
    // offset-derived `__LINE__`/`__FILE__`; may be null/empty for a no-include
    // identity pass), and the once-computed `__DATE__`/`__TIME__` INNER spellings
    // (without the surrounding quotes -- `materializePredefined` quotes them).
    std::unordered_map<std::string, PredefinedMacroDef> predefined_;
    LineMap const*                       lineMap_ = nullptr;
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
};

} // namespace

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

PreprocessResult preprocess(
    std::shared_ptr<SourceBuffer>        mainSource,
    std::shared_ptr<GrammarSchema const> schema,
    std::span<fs::path const>            includeDirs,
    std::span<fs::path const>            systemDirs,
    std::optional<ObjectFormatKind>      activeFormat,
    std::span<std::string const>         userDefines) {
    if (!mainSource || !schema) ppFatal("preprocess: null source or schema");
    if (!schema->preprocess().enabled) {
        ppFatal("preprocess: called with a schema whose preprocess pass is "
                "disabled - caller must gate on preprocess().enabled");
    }

    PreprocessResult result;
    result.diagnostics = std::make_unique<DiagnosticReporter>();

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
        for (PredefinedMacroDef const& pm : schema->preprocess().predefinedMacros) {
            if (!pm.isFunctionLike) continue;
            // FINDING-B (C21): the SHARED per-format filter (same as the pre-scan
            // value prefix / sbNameDefined / the predefined_ seed).
            if (!predefinedMacroAvailableOnActiveFormat(pm.availableObjectFormats,
                                                        activeFormat)) {
                continue;
            }
            builtinText += "#define ";
            builtinText += pm.name;
            builtinText += '(';
            for (std::size_t i = 0; i < pm.params.size(); ++i) {
                if (i != 0) builtinText += ',';
                builtinText += pm.params[i];
            }
            builtinText += ") ";
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
            auto const eq = d.find('=');
            std::string const name =
                (eq == std::string::npos) ? d : d.substr(0, eq);
            std::string const val =
                (eq == std::string::npos) ? std::string{"1"} : d.substr(eq + 1);
            cliText += "#define ";
            cliText += name;
            cliText += ' ';
            cliText += val;
            cliText += '\n';
        }
        if (!cliText.empty()) {
            auto origin = SourceBuffer::fromString(cliText, "<command-line>");
            appendWithContinuationSplice(origin->text(), origin, 0, synthText,
                                         result.lineMap);
        }
    }

    std::vector<fs::path> includeStack;
    {
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(fs::path{mainSource->name()}, ec);
        includeStack.push_back(ec ? fs::path{mainSource->name()} : canon);
    }
    // C21 (D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE, Option 2): the `#define NAME
    // VALUE\n` VALUE prefix for the include-gating pre-scan. So a `#if
    // <cmdline/predefined>` VALUE guard (`#if SQLITE_TEST >= 1`,
    // `#if __STDC_VERSION__ >= 201112L`) gating a quote-`#include` evaluates
    // correctly, the pre-scan must see the macro's VALUE -- not just its
    // definedness. Built from:
    //   (a) every command-line `--define`, parsed EXACTLY like the `<command-line>`
    //       prologue above (VALUE defaults to 1) -- so the pre-scan is more-live
    //       only IN LOCKSTEP with the authoritative pass (the one-directional-
    //       divergence invariant that keeps P0016 closed); PLUS
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
        auto const eq = d.find('=');
        std::string const name =
            (eq == std::string::npos) ? d : d.substr(0, eq);
        std::string const val =
            (eq == std::string::npos) ? std::string{"1"} : d.substr(eq + 1);
        preScanDefinePrefix += "#define ";
        preScanDefinePrefix += name;
        preScanDefinePrefix += ' ';
        preScanDefinePrefix += val;
        preScanDefinePrefix += '\n';
    }
    for (PredefinedMacroDef const& pm : schema->preprocess().predefinedMacros) {
        if (pm.isFunctionLike) continue;   // FINDING-A: never value-seed a call macro
        if (!predefinedMacroAvailableOnActiveFormat(pm.availableObjectFormats,
                                                    activeFormat)) {
            continue;
        }
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
    SynthBuilder builder{schema, includeDirs, systemDirs, activeFormat,
                         *result.diagnostics, 0, includeStack, result.fatal,
                         preScanDefinePrefix};
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
    auto prefixBuffer = SourceBuffer::fromString(
        synthText, std::string{mainSource->name()});
    result.mainSourceId = mainSource->id();

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
    DiagnosticReporter provisionalTokDiags;
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
                           includeDirs,   systemDirs,   activeFormat,
                           fs::path{mainSource->name()}.parent_path()};
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
    {
        auto byteInDeadRegion = [&](ByteOffset b) {
            for (auto const& [ds, de] : expander.deadRanges()) {
                if (b >= ds && b < de) return true;
            }
            return false;
        };
        for (ParseDiagnostic const& d : provisionalTokDiags.all()) {
            if (d.code == DiagnosticCode::P_IllegalChar
                && byteInDeadRegion(d.span.start())) {
                continue;   // dead-branch illegal char — elided, no error
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

    return result;
}

} // namespace dss
