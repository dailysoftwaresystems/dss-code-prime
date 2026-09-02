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
// FC15a (`#`/`##` operators) adds the STRINGIZE (`#`, C 6.10.3.2) and
// TOKEN-PASTE (`##`, C 6.10.3.3) operators to a function-like macro's
// replacement list. Their OPERANDS use the RAW (un-pre-expanded) argument:
// `#param` produces a single string-literal of the argument's source spelling
// (white space collapsed, `"`/`\` escaped); `a##b` concatenates the two
// adjacent tokens' spellings into ONE re-tokenized token (left-to-right, so
// `a##b##c` chains). A `#`/`##` product is a SYNTHETIC token: its spelling is
// appended to the synth text BEFORE the final buffer is frozen (config A2), so
// the product token's span slices to its real text (`"hello"` / `add3`) from
// the SAME single buffer the parser parses -- never to `#`/`##`. The `#`/`##`
// vocabulary is config-driven (`preprocess.stringizeToken`/`pasteToken`),
// default-absent for a non-C language. The FC15 paste residuals complete `##`:
// it also applies to OBJECT-like macros (`#define HW a##b` -> `ab`,
// D-PP-PASTE-OBJECT-LIKE) and to EMPTY operands via PLACEMARKERS (`J(x,)` -> `x`,
// C 6.10.3.3p2, D-PP-PASTE-PLACEMARKER); GNU `,##__VA_ARGS__` comma-elision is
// config-gated (`preprocess.variadicCommaElision`, D-PP-VARIADIC-GNU-COMMA-ELISION).
// A genuine dangling `##` (no operand token at all) still fails loud.
//
// FAIL-LOUD on every unsupported construct (function-like macro arity mismatch,
// unterminated invocation, variadic/duplicate-parameter/malformed parameter
// list, incompatible redefinition, missing quote include, include recursion
// overflow, an unterminated/mismatched conditional, a non-ICE / sizeof / float /
// string operand in `#if`, a `#` not followed by a parameter, a `##` at the
// start/end of a replacement list, a `##` product that is not a single token)
// -- never a silent pass-through or miscompile.

#include "core/export.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/header_name_matching.hpp"  // HeaderNameMatching (D-PP-HEADER-CASE-INSENSITIVE-PE)
#include "core/types/line_map.hpp"              // LineMap / LineMapSegment (the coordinate map, shared with the CU + src/lsp/)
#include "core/types/object_format_kind.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "tokenizer/token_stream.hpp"

#include <array>          // kPredefinedMacroFamilyPaths (the shared family-path table)
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>   // UserDefineSplit (c105 — the `--define` split's ONE owner)
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss {

// TF-C82 (D-PP-PRAGMA-REGISTRY): the `pragmaPackByOffset` sentinel for a token
// emitted under TWO DIFFERENT `#pragma pack` caps. Not a representable
// alignment (the channel's domain is a power of two <= 256), so it can never
// collide with a real cap.
inline constexpr std::uint32_t kPragmaPackAmbiguous = 0xFFFFFFFFu;

// D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES:
// `LineMapSegment` and `LineMap` MOVED to `core/types/line_map.hpp` (pure
// move, no logic change). They are the coordinate map BETWEEN tiers, so
// they cannot live inside the surface of one of them: `CompilationUnit`
// carries the map and `src/lsp/` reads it, and neither should have to pull
// the whole preprocessor to speak about a byte offset. Both operations are
// inline there, so this stayed a header-only dependency with no link edge.

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

    // D-PERF-1 effectiveness metric: total front-splice token-moves in the macro
    // pass; the O(n^2)->O(n) pin asserts this is <= k*N. Summed across every
    // `spliceOver` in `MacroExpander::expand`; the front-consumed-deque rewrite
    // keeps it LINEAR in the token count (zero for an identity pass or a TU with
    // no macro expansions).
    std::size_t macroTokenMoves = 0;

    // D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: the weakly-canonical paths of the
    // AUTHORITATIVELY-LIVE shipped system descriptors this preprocess run resolved
    // -- every angle `#include <h>` (or quote->angle fallback) the SynthBuilder
    // SPLICED whose splice offset the AUTHORITATIVE macro pass did NOT prove dead,
    // each expanded to its TRANSITIVE `includes` closure and DEDUPED by weakly-
    // canonical path. This set EQUALS the finish() oracle's `shippedLibDescriptors`
    // (built by the import resolver from the SAME authoritatively-live includes),
    // never a superset. EMIT-ONLY -- it changes no preprocess behavior; it surfaces
    // which descriptors' SEMANTICALLY-injected typedef surfaces are in scope so
    // `parseAndAdd_` can SEED their typedef NAMES into the binder sketch's global
    // scope BEFORE the FIRST parse. A shipped-typedef cast `(size_t)(expr)` then
    // commits as a CAST on parse 1 (no AmbiguousTypeNameCandidate -> no full-file
    // oracle reparse in UnitBuilder::finish), and because the set is oracle-aligned
    // the seed resolves EXACTLY what the reparse would, never a name it would not.
    // Empty for a TU that resolves no live system descriptor (every non-C language,
    // any C TU with only quote includes, and a descriptor reached ONLY through a
    // dead `#if 0` branch).
    std::vector<std::filesystem::path> resolvedShippedDescriptors;

    // ★★ TF-C82 (D-PP-PRAGMA-REGISTRY): the `#pragma pack` product — synth byte
    // offset of an emitted token -> the MAXIMUM MEMBER ALIGNMENT (in bytes) in
    // effect when the preprocessor emitted it. An offset ABSENT from the map
    // means "no cap", so an EMPTY map (every TU that uses no `#pragma pack` —
    // i.e. every TU before this cycle) is exactly the old behavior at zero cost.
    //
    // `#pragma pack` is the one pragma with real translation semantics that the
    // sqlite corpus REACHES: MEASURED, 40 lines across 5 TUs, and `sys/fcntl.h`'s
    // `pack(4)` region is what makes `struct log2phys` 20 bytes / align 4 instead
    // of 24 / 8 — a struct sqlite hands to `fcntl(F_LOG2PHYS)`. Dropping the
    // pragma is a wrong-ABI miscompile, not a missing warning.
    //
    // ★ KEYED PER TOKEN, NOT AS BYTE REGIONS, AND THE DIFFERENCE IS LOAD-BEARING.
    // A region list says where the PRAGMA sits; this says what was in effect when
    // a TOKEN was emitted. They disagree exactly when a composite arrives from a
    // macro REPLACEMENT LIST: its tokens carry the `#define` line's span, which
    // is nowhere near the `pack(4)` region containing the INVOCATION, so a region
    // lookup would answer "no cap" and lay the struct out wrong in silence. The
    // consumer looks up a composite specifier's FIRST TOKEN offset.
    //
    // A value of `kPragmaPackAmbiguous` means the token was emitted under TWO
    // DIFFERENT caps (a macro replacement expanded in two pack regions). The
    // preprocessor records that and says nothing: it cannot know which offsets
    // are used as a LAYOUT KEY, and MEASURED, erroring here refuses programs
    // clang compiles (a shared MEMBER macro used in two regions, where every
    // composite is still anchored on an unambiguous `struct` keyword). The
    // semantic tier turns it into `S_PragmaPackAmbiguous` only when a composite
    // actually lands on such an offset.
    std::unordered_map<std::uint32_t, std::uint32_t> pragmaPackByOffset;

    // ★★ TF-C85 (`optimizerControl`): the `#pragma optimize` product — the synth
    // byte offsets of tokens the preprocessor emitted inside a
    // `#pragma optimize("", off)` region. An offset ABSENT from the set means
    // "optimize normally", so an EMPTY set (every TU that uses no
    // `#pragma optimize` — i.e. every TU before this cycle) is exactly the old
    // behavior at zero cost.
    //
    // Keyed PER TOKEN for the identical reason `pragmaPackByOffset` is: a region
    // list says where the PRAGMA sits, while this says what was in effect when a
    // TOKEN was emitted, and the two disagree exactly when a function definition
    // arrives from a macro replacement list. The consumer (the semantic tier's
    // Pass 1.5) looks up a function DECLARATION's leftmost emitted token.
    //
    // A token emitted BOTH inside and outside a region resolves to OFF and says
    // nothing — see `noOptimizeByOffset_` in the .cpp for why that is a sound
    // resolution here and was NOT one for `pack`.
    std::unordered_set<std::uint32_t> pragmaNoOptimizeByOffset;

    // Build a remap closure usable by `DiagnosticReporter::remapBuffers`:
    // it rewrites any diagnostic whose buffer is the synth buffer to the
    // origin (buffer id + offset-shifted span). Diagnostics on other buffers
    // pass through untouched.
    [[nodiscard]] std::function<void(BufferId&, SourceSpan&)> makeRemap() const;

    // ── [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: the same rewrite over a WHOLE
    //    diagnostic ────────────────────────────────────────────────────────────
    //
    // Primary span, every related location, AND — when the diagnostic's subject
    // is a macro-expansion PRODUCT token — an appended
    // `note: expanded from macro 'X'` at that macro's `#define`. The position
    // half is the same `remapOnePosition` `makeRemap` uses; only the annotation
    // needs the diagnostic, which a `(BufferId&, SourceSpan&)` closure cannot
    // reach.
    //
    // ★ BOTH SHAPES EXIST BECAUSE BOTH CONSUMERS DO. `makeRemap`'s callers —
    // the LSP position map, the shipped-descriptor refs, every post-parse tier's
    // `remapPreprocessedPositions` — convert a bare coordinate and have no
    // diagnostic to annotate. `DiagnosticReporter::remapBuffers` accepts either
    // and dispatches on the callable's signature, so `Tree::remapDiagnostics`
    // and every other call site is unchanged.
    [[nodiscard]] std::function<void(ParseDiagnostic&)> makeDiagnosticRemap() const;

    // The Eof token that terminates `tokens`.
    //
    // ★ USE THIS, NEVER `tokens.back()` ([[D-PP-RESULT-CONTRACT-SINGLE-EXIT]]).
    // A consumer that needs an Eof-only stream (the D-PP-FATAL-HALTS-PARSE arm
    // in `compilation_unit.cpp`) used to reach for `tokens.back()` under a
    // comment asserting the vector is "Eof-terminated by contract" — a contract
    // the consumer had no way to verify. When a producer path broke it, the
    // result was not a diagnostic but a SEGFAULT (MEASURED TF-C115: rc 139 /
    // 0xC0000005 on the predefined-macro-collision abort, with no output at
    // all). This accessor is the ONE place the contract is checked on the read
    // side: `preprocess()`'s single exit makes the violation impossible, and
    // this makes any residual violation LOUD and named instead of undefined.
    [[nodiscard]] Token const& eofToken() const;
};

// ── TF-C74 (D-CONFIG-PER-ARCH-PREDEFINED-MACROS) + TF-C97
//    (D-PP-FORMAT-DATA-MODEL-PREDEFINES): the effective list ─────────────
//
// The ONE effective-predefined-macro list.
//
// Predefined macros come from THREE independent config families — and the three
// are exactly the project's agnosticism axes, which is why there are three and
// not two:
//   • the LANGUAGE (`preprocess.predefinedMacros` in `<lang>.lang.json` —
//     `__LINE__`, `_WIN32`, …): the SOURCE half.
//   • the TARGET (`predefinedMacros` in `<arch>.target.json` — `__aarch64__`,
//     `__x86_64__`, …): the PROCESSOR half. Per-CPU facts.
//   • the FORMAT (`predefinedMacros` in `<name>.format.json` —
//     `__LP64__`/`_LP64`): the OBJECT-FORMAT half. Facts that are neither the
//     language's nor the CPU's, because ONE CPU answers differently on two of
//     its own formats (x86_64 is LP64 under elf64/macho64, LLP64 under pe64).
// All three are paired only at COMPILE time, when a (language, target, format)
// triple actually exists, so this is where the merge — and the collision check
// — must live.
// The THREE config families' `predefinedMacros` locations, in merge order:
// language, then target, then object format. ONE copy, shared by the collision
// diagnostic (which must name BOTH declaring files) and by the `requires`
// satisfaction diagnostic (which must name the declaring file and the row).
//
// ★ Each entry carries the FILE FAMILY as well as the JSON pointer, because the
// target and format families use the SAME pointer (`/predefinedMacros`) — a
// bare pointer names two different files identically, which is exactly the
// "which file do I edit?" failure these strings exist to prevent.
//
// ★ AND IT IS ONE TABLE RATHER THAN THREE STRINGS AT THREE CALL SITES: a second
// copy is a copy that can disagree, and a diagnostic naming the wrong config
// file is worse than one naming none.
inline constexpr std::array<std::string_view, 3> kPredefinedMacroFamilyPaths{
    std::string_view{"<lang>.lang.json /preprocess/predefinedMacros"},
    std::string_view{"<arch>.target.json /predefinedMacros"},
    std::string_view{"<name>.format.json /predefinedMacros"}};

struct DSS_EXPORT MergedPredefinedMacros {
    // The effective list: language entries FIRST, then target, then format,
    // each in declaration order, with the per-entry `availableObjectFormats`
    // filter ALREADY APPLIED. This is the single list every seed site iterates,
    // which is why the filter is applied exactly ONCE, here — the four seed
    // sites can no longer each apply their own copy and drift.
    std::vector<PredefinedMacroDef> effective;
    // One message per colliding NAME, each naming BOTH declaring config paths.
    // NON-EMPTY ⇒ the merge FAILED: `effective` is not usable and the caller
    // must emit `C_ConflictingPredefinedMacro` and abort the pass. There is no
    // "merge anyway" mode — a silent last-writer-wins in any direction is
    // the exact wrong-value miscompile this check exists to prevent.
    std::vector<std::string> conflicts;
};

// Merge the language, target and format predefined-macro lists for
// `activeFormat`.
//
// Properties (each pinned by a test):
//  (a) a NAME present in more than one list is a conflict, compared BEFORE the
//      format filter — so a `["pe"]`-gated language `_WIN32` still collides
//      with an ungated target `_WIN32`. Gating is about which formats SEE a
//      macro, not about who OWNS the name; deferring the check until after the
//      filter would let a collision hide on every leg but one. ALL THREE PAIRS
//      are scanned (language×target, language×format, target×format): with
//      three families a scan that covered only two would leave one pair able to
//      ship a silent last-writer-wins.
//  (b) the per-format filter is applied ONCE, here. (An entry with an EMPTY
//      `availableObjectFormats` is available on every format; a non-empty set
//      restricts it to those formats; absent an active format only a
//      universal entry survives.)
//  (c) order is stable: language entries, then target, then format.
//  (d) EMPTY `targetMacros` + EMPTY `formatMacros` yields exactly the
//      language-only list the pre-TF-C74 engine computed — the no-regression
//      invariant, which is also the LSP / FFI-header-parser configuration.
//  (e) D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC: every group in
//      `exclusiveGroups` may contribute AT MOST ONE effective macro. Two or
//      more ⇒ a conflict naming the group's members, the group's `reason`, and
//      the OFFENDING FORMAT — so the message identifies the leg to fix rather
//      than merely asserting that some rule was broken.
//      ★ Checked AFTER the format filter, unlike (a): the defect class is a
//      pair that is individually legitimate and becomes contradictory only
//      once a particular format makes both live — typically one member gated
//      to a single format and the other un-gated. BEFORE the filter every leg
//      looks equally guilty and none can be named.
//      ★ EMPTY `exclusiveGroups` is exactly the unchecked pre-pin behaviour —
//      which is what makes deleting the shipped group a genuine red-on-disable
//      rather than a no-op. Single-family callers (the dump's origin
//      attribution) pass `{}` because the authoritative check already ran over
//      the full merge; re-running it per family could only under-report.
[[nodiscard]] DSS_EXPORT MergedPredefinedMacros mergePredefinedMacros(
    std::span<PredefinedMacroDef const> languageMacros,
    std::span<PredefinedMacroDef const> targetMacros,
    std::span<PredefinedMacroDef const> formatMacros,
    std::optional<ObjectFormatKind>     activeFormat,
    std::span<PredefinedMacroExclusionGroup const> exclusiveGroups = {});

// ── c105 (D-PP-USER-DEFINE): the ONE owner of `--define NAME[=VALUE]` ─────
//
// Split ONE command-line `--define` entry into the NAME and the VALUE the
// preprocessor will actually seed. An entry with NO `=` takes the value `1`
// (the gcc `-DNAME` rule); an entry with a `=` takes everything after the
// FIRST `=` verbatim, EMPTY INCLUDED (`--define NAME=` defines NAME as empty,
// which is a real and different thing from defining it as `1`).
//
// ★ WHY THIS IS A FUNCTION AND NOT THREE COPIES OF FOUR LINES. The
// "defaults to 1" rule had TWO independent implementations inside
// `preprocessRun` — the `<command-line>` PROLOGUE and the include-gating
// PRE-SCAN prefix — held in agreement only by a comment on the second one
// saying it parses "EXACTLY like the prologue above". A drift between them is
// not a cosmetic bug: the pre-scan decides whether a `#if NAME`-gated
// quote-`#include` resolves, so a pre-scan that read a different value than
// the authoritative pass would silently include (or silently skip) a header.
// `--dump-predefined-macros` needed the same rule a THIRD time — to report
// what a `--define` actually contributes — and a third copy is how a
// verification instrument ends up disagreeing with the thing it verifies. One
// function, three callers, no convention to keep.
//
// `name` and (when stated) `value` are views INTO `entry` — the caller keeps
// `entry` alive. The DEFAULT value is a view of a string literal with static
// storage, so it outlives any caller.
struct DSS_EXPORT UserDefineSplit {
    std::string_view name;
    std::string_view value;
    // false ⇒ the entry carried no `=` and `value` is the DEFAULT. Reported by
    // `--dump-predefined-macros` so the operator can tell "you asked for 1"
    // apart from "nothing was stated and 1 is what the rule supplies".
    bool             valueWasStated = false;
};
[[nodiscard]] DSS_EXPORT UserDefineSplit
splitUserDefine(std::string_view entry) noexcept;

// FC15b (C 6.10.8.1): the translation DATE and TIME spellings, from ONE read of
// the wall clock. Returned WITHOUT the surrounding quotes (`__DATE__`'s
// materializer wraps them) — `date` is the C-mandated `"Mmm dd yyyy"` with a
// SPACE-padded day (`Jun  4 2026`), `time` is `hh:mm:ss` zero-padded.
//
// ★ ONE CLOCK READ, BOTH FIELDS. `__DATE__` and `__TIME__` must describe the
// SAME instant — two independent reads can straddle a second (or midnight) and
// ship a TU whose two timestamps disagree. Returning both from one call makes
// that impossible for every caller, rather than being a rule each caller has to
// know.
//
// Defensive: a null `std::localtime` (impossible in practice) leaves BOTH
// strings empty — the materializer then produces `""`, never a crash and never
// a fabricated date.
struct DSS_EXPORT TranslationTimestamp {
    std::string date;   // "Mmm dd yyyy", unquoted; empty iff the clock failed
    std::string time;   // "hh:mm:ss",    unquoted; empty iff the clock failed
};
[[nodiscard]] DSS_EXPORT TranslationTimestamp translationTimestamp();

// How much work the per-FILE pre-scan actually did.
//
// ★★★ THIS EXISTS BECAUSE THE PROPERTY
// [[D-PERF-PP-EVERY-INCLUDE-RE-READS-AND-RE-TOKENIZES-THE-SAME-HEADER]] PINS IS
// A COUNT, AND IT WAS BEING PINNED WITH A CLOCK. The defect was that the read +
// continuation-splice + tokenize of a header was paid per OCCURRENCE of an
// `#include` naming it rather than once per FILE. That is a statement about HOW
// MANY TIMES the work happened — so the honest instrument is a counter, not a
// stopwatch. ✔MEASURED on CI run 33156833090: the ratio pin that stood in for
// this counter read x1.048 against a bound of 0.85 on `linux-gcc-release` and
// PASSED on `linux-arm64-gcc-release` in the same run at the same commit, which
// is a property of the runner rather than of the compiler
// [[D-TEST-PP-NO-REWORK-PINS-A-COUNT-WITH-A-WALL-CLOCK-RATIO]].
//
// ⚠ COUNTERS, NOT A CACHE POLICY SURFACE. There is nothing to configure here
// and nothing a caller may switch off: the memo is a memo of a pure function,
// so a hit and a miss cannot disagree. These are OBSERVATIONS of it, in the
// shape `substrate::PhaseTimers` already established for phase accounting —
// static accessors, process-wide, plus a `reset()` that exists for test
// isolation and that the driver never calls.
//
// ⓘ `builds` counts WORK DONE, not misses: two threads racing on a cold header
// both build it and the first to publish wins, so `builds` may exceed the
// number of distinct files by the number of lost races. It is exact on the one
// thread a test uses. A request whose file cannot be READ counts as neither —
// nothing was memoized and nothing was served.
class DSS_EXPORT PreScanMemoCounters {
public:
    struct Row {
        // Files continuation-spliced and tokenized by the pre-scan.
        // ⚠ NOT "files read". The memo is keyed on the file's CONTENT DIGEST —
        // D-PP-PRE-SCAN-MEMO-SERVES-A-SAME-SIZE-EDIT-INSIDE-ONE-TIMESTAMP-TICK-STALE
        // — so EVERY request reads its file in order to have bytes to key on,
        // and only a request that then does the splice and the tokenize counts
        // here. `builds + hits` is therefore the read count, and `builds` alone
        // is the memoized-work count.
        std::uint64_t builds = 0;
        // Pre-scan requests answered from the memo without doing that work.
        std::uint64_t hits = 0;
    };

    [[nodiscard]] static Row read() noexcept;

    // Zero both counters. Test isolation only — the driver never resets.
    static void reset() noexcept;
};

// Run the preprocessor over `mainSource` under `schema`. Precondition:
// `schema->preprocess().enabled` is true (the caller gates on it; calling
// with a disabled schema is a usage error and fatal-asserted). `includeDirs`
// is the quote-include search path (the including file's own directory is
// always tried first, mirroring the import resolver).
//
// FC15c (Option A): `systemDirs` is the ANGLE-include / system-header search
// path (the analogue of C's /usr/include; DSS ships LANGUAGE-NEUTRAL JSON
// descriptors there, e.g. `stdio.json`). It feeds the `__has_include(<h>)`
// existence test so it agrees with what the post-parse import resolver does for
// `#include <h>`. Defaults to {} so the ~15 test callers + helper compile
// unchanged; the ONE production call site (compilation_unit.cpp) threads its
// `systemDirs_` member.
// c9 (Phase-2): `activeFormat` is the active compile target's object-format, when
// known (a real per-target compile). It makes `__has_include(<h>)` per-target
// truthful — a header whose shipped descriptor declares `availableObjectFormats`
// not containing this format reports NOT available, agreeing with the `#include`
// semantic gate. Defaults to nullopt (LSP / direct-API / tests / non-C languages)
// → pure-existence behavior, identical to before. Because `__has_include` (and the
// descriptor macro-splice) now depend on it, the front-end must be built ONCE PER
// DISTINCT object-format (the driver groups targets by kind; nullopt builds once).
// c105 (D-PP-USER-DEFINE): `userDefines` are the CLI `--define NAME[=VALUE]`
// entries, verbatim. Each lowers to a `#define NAME VALUE` line (VALUE
// defaults to `1`) in a synthetic "<command-line>" PROLOGUE prepended to the
// synth stream — the C/gcc model ("as if #define appeared before the first
// source line"). The ORDINARY directive handler then owns everything: name
// validation, the C 6.10.8.1 predefined-collision guard (a user may not
// silently flip a profile macro like `_MSC_VER` — loud), the 6.10.3p2
// duplicate policy (identical dup tolerated; conflicting dup loud), and
// #undef-ability (a -D macro is an ordinary macro). Function-like predefined
// macros (PredefinedMacroDef.isFunctionLike) ride the same mechanism via a
// "<built-in>" prologue. Defaults to {} — every existing caller unchanged.
// TF-C74: `targetPredefinedMacros` is the ACTIVE TARGET's `predefinedMacros`
// list (`TargetSchema::predefinedMacros()`) — the per-architecture identity
// macros. TF-C97: `formatPredefinedMacros` is the ACTIVE OBJECT FORMAT's
// (`ObjectFormatSchema::predefinedMacros()`) — the data-model face,
// `__LP64__`/`_LP64`. Note these are the format SCHEMA's rows, i.e. per format
// FILE, while `activeFormat` is only the format KIND; the two are independent
// inputs and a caller must not try to derive one from the other. All three
// lists are merged ONCE at entry (`mergePredefinedMacros`), producing the
// single effective list all four seed sites then iterate. A name declared by
// more than one family is FATAL (`C_ConflictingPredefinedMacro`), never
// silently resolved. Both default to {} ⇒ output byte-identical to pre-TF-C74,
// so every existing caller compiles and behaves unchanged.
// D-PP-HEADER-CASE-INSENSITIVE-PE: `headerNameMatching` is the ACTIVE OBJECT
// FORMAT's declared header-NAME case rule (`*.format.json`'s
// `headerNameMatching`), applied by EVERY include search this pass performs —
// the angle `#include <h>` arm, the quote arm, both `__has_include` callbacks
// (pre-scan + authoritative), the descriptor macro-splice, and `#embed`. It is
// a SEPARATE input from `activeFormat`: that is a format KIND, and deriving a
// case rule from a kind would be the `if (kind == Pe)` identity branch the
// agnosticism bar forbids. Defaults to `kDefaultHeaderNameMatching`
// (case-SENSITIVE — the conservative POSIX rule) so the LSP / direct-API /
// test callers behave exactly as a conforming POSIX toolchain would.
[[nodiscard]] DSS_EXPORT PreprocessResult preprocess(
    std::shared_ptr<SourceBuffer>        mainSource,
    std::shared_ptr<GrammarSchema const> schema,
    std::span<std::filesystem::path const> includeDirs,
    // ★ REQUIRED, AND ITS POSITION IS LOAD-BEARING. It sits in the required
    // block — ahead of every defaulted parameter — because C++ only lets a
    // parameter be un-defaultable if nothing before it is defaulted, and a
    // defaulted case rule is a silent choice at every call site. "The compiler
    // enforces that every caller states its policy" is only true when there is
    // nothing to fall back to. A caller with no active object format passes
    // `kDefaultHeaderNameMatching` EXPLICITLY, which makes that decision
    // greppable instead of invisible.
    HeaderNameMatching                   headerNameMatching,
    // ★ REQUIRED, AND IN THE REQUIRED BLOCK FOR THE SAME REASON AS
    // `headerNameMatching` ABOVE. The operator's diagnostic volume budget,
    // handed to `result.diagnostics`, to `provisionalTokDiags`, and through
    // `tokenizeToPP` to the synth-buffer `Tokenizer`. Defaulting it would put
    // the library's 1000/50 back at every call site invisibly, which is the
    // defect D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE records. A
    // caller with no operator budget passes `DiagnosticBudget::libraryDefault()`.
    DiagnosticBudget                     budget,
    std::span<std::filesystem::path const> systemDirs = {},
    std::optional<ObjectFormatKind>      activeFormat = std::nullopt,
    std::span<std::string const>         userDefines  = {},
    std::span<PredefinedMacroDef const>  targetPredefinedMacros = {},
    std::span<PredefinedMacroDef const>  formatPredefinedMacros = {});

// FC17.9(h) (`#embed`; the size-cap boundary of D-PP-EMBED): a PURE budget
// check for the cycle-1 `#embed` splice. The splice materializes the resource as
// ~2 tokens/byte (an IntLiteral + a Comma) across the body/out/result.tokens
// vectors + the parser's `fromTokens` copy, so a large resource (tens–hundreds
// of MiB -- the exact use case a real `#embed` targets) OOM-CRASHES long before
// the 4 GiB `ByteOffset` text wall. An OOM is neither fail-loud nor graceful, so
// the handler gates the resource's byte COUNT through this helper FIRST: it
// returns a diagnostic MESSAGE when `byteCount` exceeds `kEmbedMaxResourceBytes`
// (the caller emits it as `P_PreprocessorEmbed` on the directive word, naming the
// streaming deferral), else nullopt. Taking a COUNT (not a file) makes the
// red-on-disable unit test call it directly with `cap+1` -- no giant fixture. A
// real `limit`-aware streaming splice belongs with the deferred parameters cycle.
inline constexpr std::size_t kEmbedMaxResourceBytes = 16u * 1024u * 1024u; // 16 MiB
[[nodiscard]] DSS_EXPORT std::optional<std::string>
embedResourceSizeError(std::size_t byteCount);

} // namespace dss
