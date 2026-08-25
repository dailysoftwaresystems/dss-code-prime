#pragma once

#include "core/export.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/preprocess_config.hpp"   // PredefinedMacroDef
#include "program/cli_args.hpp"

#include <expected>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ── `--dump-predefined-macros` — DSS's answer to `gcc -dM -E` ────────────────
//
// ★ WHY THIS EXISTS. The project's standing position is that DSS never defines a
// C23 conditional-feature NON-SUPPORT macro (`__STDC_NO_THREADS__`,
// `__STDC_NO_VLA__`, `__STDC_NO_ATOMICS__`, `__STDC_NO_COMPLEX__`) — the goal is
// to REALIZE each feature, not to announce a gap. That claim was true and
// UNVERIFIABLE at the same time: DSS shipped no way to ask a build what it
// predefines, so the only evidence was three config files and four comments. An
// unverifiable invariant is one config edit away from being quietly false. This
// is the instrument that makes it checkable — and, being checkable, testable
// (see `tests/program/test_dump_predefined_macros.cpp`, which asserts the
// absence of all four across EVERY shipped object format).
//
// ★ THE SINGLE-OWNER CONSTRAINT — the whole architectural point.
// `mergePredefinedMacros(language, target, format, activeFormat)` already
// returns the ONE effective list every preprocessor seed site iterates, with the
// per-entry `availableObjectFormats` filter applied EXACTLY ONCE and the
// three-way collision scan done. This dump prints THAT list by CALLING THAT
// FUNCTION. It never re-walks the three config families, because a verification
// instrument that computes its own answer can DISAGREE with the thing it
// verifies — and a dump that disagrees with the compiler is worse than no dump,
// since it would be believed. The same rule governs the two other facts the
// output needs: the `--define` NAME/VALUE split comes from `splitUserDefine` and
// the `__DATE__`/`__TIME__` spellings from `translationTimestamp`, both in the
// preprocess tier, both shared with the real pass.
//
// ★ WHAT IT REFUSES TO PRINT. `line` / `file` kinds are OFFSET-DERIVED — their
// value is a function of the invocation site, so there is no single value a dump
// could honestly report. Function-like entries (`isFunctionLike`) are the same:
// their expansion depends on the arguments at the call. Both print their KIND
// and say the value is not single-valued. A FABRICATED value would be strictly
// worse than none, because a wrong value is trusted and a missing one is not.
//
// Agnosticism: nothing in this tier compares a macro NAME, a language name, an
// architecture name or a format name against a literal. Per-entry behaviour keys
// off `PredefinedMacroDef::kind` and `isFunctionLike`; the origin label comes
// from which config family the entry was submitted under; the object-format
// spelling comes from `objectFormatKindName`. The schema names in the output are
// echoes of what the operator typed.

namespace dss {

// ── The output vocabulary ────────────────────────────────────────────────────
//
// LINE SHAPE (one per effective macro), `key=value` fields separated by single
// spaces, with `value=` LAST so a spelling containing spaces is the unambiguous
// remainder of the line:
//
//   predefined-macro origin=<o> kind=<k> form=<f> name=<NAME> value=<...>
//
// A fixed leading marker (never a regex over prose) so a machine reader selects
// these lines by prefix — the same discipline as the `dsscp: artifact`
// report line. Every field is a single token except `value`.
inline constexpr std::string_view kPredefinedMacroLineMarker = "predefined-macro";

// The section header preceding each target's block. Names the FULL resolved
// triple plus the effective COUNT, so a collapsed enumeration (a section that
// answered for nothing) is visible in the output itself rather than inferred
// from a short list.
inline constexpr std::string_view kPredefinedMacroHeaderMarker =
    "dsscp: predefined-macros";

// A NOTE line, emitted after a section's macro lines. Its own marker (never the
// macro marker) so a machine reader selecting macro lines by prefix never picks
// one up by accident.
//
// ★ WHY THE DUMP NEEDS A "I REPORT THIS, I DO NOT JUDGE IT" CHANNEL. A `--define`
// may name a macro a config already declares, and the two do NOT stack — the
// compile path REFUSES such a build (MEASURED; see the note's construction site
// in the .cpp for the four arms and the two distinct diagnostics). This dump runs
// no directive handler and has no token stream, so it can neither reproduce that
// verdict nor honestly present the pair as fine. Re-deriving the rule here would
// be a SECOND owner of a decision that routes on `isFunctionLike` and lives in
// the handler. So the dump STATES the condition, names both governing clauses,
// and leaves the verdict where it belongs.
inline constexpr std::string_view kPredefinedMacroNoteMarker =
    "dsscp: predefined-macros-note";

// The `origin=` field's closed vocabulary — WHICH CONFIG FAMILY (or the command
// line) declared the entry. This is what makes the output an AUDIT instrument
// rather than a list: "`_WIN32` is defined" is not actionable, "`_WIN32` is
// defined by the LANGUAGE config" names the file to edit.
enum class PredefinedMacroOrigin : std::uint8_t {
    Language,     // <lang>.lang.json  /preprocess/predefinedMacros
    Target,       // <arch>.target.json /predefinedMacros
    Format,       // <name>.format.json /predefinedMacros
    CommandLine,  // --define NAME[=VALUE]
};

[[nodiscard]] DSS_EXPORT std::string_view
predefinedMacroOriginName(PredefinedMacroOrigin o) noexcept;

// The `value=` field when there IS no single value. THREE DIFFERENT reasons, and
// they are spelled differently on purpose: an operator who sees one needs to
// know which question to ask next ("where is it invoked?" vs "with what
// arguments?" vs "how many times has it already been read?"). No spelling can be
// mistaken for a macro value: all begin with the reserved `<no-single-value`
// prefix, which no config value has (they are integer-constant spellings, string
// literals, or empty).
inline constexpr std::string_view kNoSingleValueOffsetDerived =
    "<no-single-value: derived from the invocation offset at each use>";
inline constexpr std::string_view kNoSingleValueFunctionLike =
    "<no-single-value: function-like — expands against its arguments at each "
    "call>";
// D-CSUBSET-COUNTER-MACRO-NOT-EXPANDED. Deliberately NOT folded into the
// offset-derived spelling: an offset-derived macro is reproducible from its
// POSITION, and this one is not — the same site yields a different value on a
// second expansion. That distinction is the entire semantics of `__COUNTER__`,
// so a dump that blurred it would misdescribe the one thing worth knowing.
inline constexpr std::string_view kNoSingleValueStateful =
    "<no-single-value: per-translation-unit counter — advances at each "
    "expansion>";

// ── ONE triple's dump request ───────────────────────────────────────────────
//
// The three config families arrive as SPANS, exactly as `mergePredefinedMacros`
// takes them, so this type cannot be constructed by re-deriving a list: a caller
// hands over what it loaded. The CLI entry point below loads them from the
// resolved schemas; a test hands over literals. Same code path either way, which
// is what makes the conflict arm reachable in a test at all (no shipped config
// collides, by construction).
struct DSS_EXPORT PredefinedMacroDumpRequest {
    // Echoed into the section header. Opaque strings — never compared to a
    // literal anywhere in this tier.
    std::string_view languageName;
    std::string_view targetName;
    std::string_view formatName;

    std::span<PredefinedMacroDef const> languageMacros;
    std::span<PredefinedMacroDef const> targetMacros;
    std::span<PredefinedMacroDef const> formatMacros;

    // The ACTIVE object format's kind — the per-entry `availableObjectFormats`
    // filter's input. `nullopt` means "no active format", under which the merge
    // keeps only universal entries; the CLI never passes `nullopt` (it demands a
    // `--target`), but the merge's contract admits it and this type must not
    // pretend otherwise.
    std::optional<ObjectFormatKind> activeFormat;

    // D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC: the language's mutual-exclusion
    // groups, forwarded to the merge so this instrument REFUSES an impossible
    // identity for exactly the triples a real compile refuses. A dump that
    // cheerfully printed a set the compiler rejects would be the instrument
    // disagreeing with the thing it exists to describe — and this dump is how
    // the co-definition defect was found in the first place.
    std::span<PredefinedMacroExclusionGroup const> exclusiveGroups;

    // `--define NAME[=VALUE]` entries, VERBATIM as the CLI captured them. Split
    // by `splitUserDefine` at render time — the same function the preprocessor's
    // `<command-line>` prologue uses.
    std::span<std::string const> userDefines;
};

// Render ONE triple's effective set.
//
// SUCCESS → the section text (header line + one line per effective macro + one
// line per `--define`), newline-terminated, ready to write verbatim.
//
// FAILURE → the merge's OWN conflict messages, unmodified (each names both
// declaring config paths). `MergedPredefinedMacros::conflicts` non-empty means
// `effective` is documented UNUSABLE, so nothing partial is returned: a dump
// that printed the surviving half of a colliding set would be the silent
// last-writer-wins the collision check exists to prevent, wearing the disguise
// of a verification tool.
[[nodiscard]] DSS_EXPORT std::expected<std::string, std::vector<std::string>>
renderPredefinedMacroDump(PredefinedMacroDumpRequest const& req);

// ── The CLI entry point ─────────────────────────────────────────────────────
//
// Resolve `args.languageName` + EVERY `args.targets` entry to real schemas,
// render each section, and write them to `out`. Returns the process exit code:
// 0 = every section rendered and printed; 1 = any failure, with the reason on
// `err` and NOTHING written to `out`.
//
// ★ ALL-OR-NOTHING ACROSS TARGETS. Every section is rendered BEFORE any is
// printed, so a conflict (or an unloadable schema) on the third target cannot
// leave the first two on stdout looking like a complete answer. Same argument as
// the per-section rule above, one level up.
//
// Streams are parameters, not `std::cout`/`std::cerr` captures, so the whole
// path is exercisable in-process by a unit test without spawning the CLI.
[[nodiscard]] DSS_EXPORT int dumpPredefinedMacros(CliArgs const& args,
                                                 std::ostream&   out,
                                                 std::ostream&   err);

} // namespace dss
