#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"

#include <string_view>

// ★★★ THE LANGUAGE↔TARGET ARCHITECTURE GATE
// (D-ISA-LANGUAGE-BOUND-TO-ARCHITECTURE).
//
// THE RULE, IN ONE SENTENCE: some source languages are inherently bound to an
// instruction-set architecture; most are portable. A language that declares an
// `isa` emits for that architecture and NOTHING ELSE — `asm-x86_64-att` cannot
// be assembled for AArch64, and that is what the language IS, not a list
// someone maintains. A language that declares NO `isa` is portable and builds
// for every target, which is the default and the overwhelmingly common case.
//
// ⓘ THE SIBLING FILE ANSWERS A DIFFERENT QUESTION.
// `cross_validate_target_format.hpp` asks "do this TARGET and this FORMAT
// agree?" and answers it from per-format `machine` codes reached through a
// table keyed on the target NAME. This file asks "may this LANGUAGE be
// compiled for this TARGET?" and answers it by comparing two DECLARED values —
// `GrammarSchema::isa()` and `TargetSchema::isa()` — for equality. The two
// gates are peers, they run at the same chokepoint, and neither subsumes the
// other: a format-kind can no more separate architectures
// (`elf64-x86_64-linux-exec` and `elf64-aarch64-linux-exec` are BOTH
// `kind: "elf"`) than a machine code can say what a language emits.
//
// ★ WHY THIS IS NOT THE PER-DEPENDENCY TARGET ENUMERATION THAT WAS REJECTED
// (operator, 2026-08-14). That design read a project's `targets[]` — "the
// platforms a project builds for ITSELF", a BUILD LIST — as a CAPABILITY
// CLAIM, so a portable dependency that merely forgot to list an architecture
// would refuse a legitimate consumer. An ISA binding cannot drift that way. It
// is ONE fact about the language, and the acceptance criterion of this design
// is mechanical: **a new target document declaring an existing `isa` value
// satisfies every language already bound to it, with no edit to any language
// config and no code change.** A design that needs a language edit when a
// target arrives has been rebuilt into the rejected one.
//
// ⓘ EQUALITY ONLY — deliberately no subset/superset lattice, no "x86-64 also
// runs 32-bit x86" reasoning. Any such relation would be precisely the
// capability claim this axis refuses to make.

namespace dss {

// Does `language` emit for an architecture `target` executes?
//
// TRUE when the language declares NO `isa` (portable — the common case, and
// the reason this gate costs nothing for C, T-SQL and every non-assembly
// language), or when both sides declare the SAME value.
//
// FALSE when the language declares an `isa` and the target declares a
// different one, AND — fail-CLOSED — when the language declares one and the
// target declares NONE: an undeclared target cannot be SHOWN to satisfy the
// binding, and guessing "probably fine" is how x86 text reaches an AArch64
// opcode table.
//
// Reads two DECLARED strings. No name, no format kind, no machine code.
[[nodiscard]] DSS_EXPORT bool
languageTargetIsaCompatible(GrammarSchema const& language,
                            TargetSchema const&  target) noexcept;

// The gate. Returns `languageTargetIsaCompatible(...)`, and on false emits
// `D_LanguageTargetIsaMismatch` (0xD02A) through `reporter`, naming the
// language, its declared ISA, the target spec, and the target's declared ISA
// (or that it declares none).
//
// `languageName` is the name the manifest/CLI used to select the grammar —
// the string the operator can act on — rather than the grammar's internal
// `language.name`, which is a different spelling (`c` vs `C`).
//
// `subject` prefixes the message to say WHOSE pairing is being refused. Empty
// for a root build; the resolver passes `"dependency '<manifest path>'"` so a
// refusal points at the manifest that declared the binding rather than at the
// project the operator invoked. The gate is otherwise IDENTICAL on both arms
// by construction — one predicate, one message body, so the two arms cannot
// drift into disagreeing about the same pair.
//
// The diagnostic takes `DiagnosticDelivery::Guaranteed` (it is the only
// statement of why this pairing stopped) and is deliberately NOT a member of
// `kUnsuppressableCodes` — see the code's allocation note for the verdict and
// the prong it fails.
[[nodiscard]] DSS_EXPORT bool
crossValidateLanguageTarget(GrammarSchema const& language,
                            std::string_view     languageName,
                            TargetSchema const&  target,
                            std::string_view     targetSpec,
                            std::string_view     subject,
                            DiagnosticReporter&  reporter);

} // namespace dss
