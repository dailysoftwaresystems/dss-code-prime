#pragma once

// ★★★ THE TEMPLATE TEXT FORMS A REFERENCE EXPANDS BEFORE ITS ASSEMBLER EVER SEES
// THE TEXT (P68 round 8, D-ASM-TEMPLATE-FORMS-A-REFERENCE-EXPANDS-REFUSED).
//
// An extended inline-asm template is not assembler input as written: gcc and
// clang rewrite some `%` forms and, on x86, the `{…|…}` dialect alternatives,
// and only the RESULT reaches gas. ✔MEASURED 2026-09-23 (gcc 13.3.0 and clang
// 18.1.3, each form inside `.ascii "X<form>Y"` so the assembler accepts the
// line and only the form is under test):
//   • `%=`  — a number unique to each asm INSTANCE, the same everywhere in one
//             template (`%=%=` → `1414` under gcc, `00` under clang): all four
//             (gcc/clang × x86_64/aarch64) expand it. GNU C's form, declared per
//             dialect all the same (see `AsmTemplateTextForms`).
//   • NOTHING is expanded in a BASIC template (no colon) by either reference —
//             `%=`, `{a|b}`, `%{` and `%%` all reach the assembler verbatim.
//   • `%{` `%}` → `{` `}`: gcc x86, clang x86 AND clang aarch64 (gcc aarch64
//             refuses; the union accepts on both).
//   • `%|` → `|`, `%*` → `*`, and `%;` `%+` `%^` `%!` → nothing: gcc x86 only.
//   • `%~`  → `f`, or `i` under `-mavx2`: gcc x86 only — the one form whose
//             expansion reads a TARGET fact (an ISA feature).
//   • `{a|b|c}` → `a` on x86 (gcc and clang: the AT&T alternative is the
//             first), a lone `{x}` → `x`, NESTED alternatives refused by both;
//             on aarch64 the braces are literal text (both).
// ⇒ which forms exist and what each becomes is DIALECT data (x86 and aarch64
// differ on every row but `%=`), declared in the dialect's `assembly` block;
// this header holds the shapes and the one expansion, and names no form.

#include "core/export.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

// A form with ONE fixed expansion (`%{` → `{`, `%;` → nothing).
struct AsmTemplateFixedForm {
    std::string form;
    std::string text;
};

// A form whose expansion depends on whether the TARGET declares a feature
// (`%~` → `i` with `avx2`, `f` without).
struct AsmTemplateFeatureForm {
    std::string form;
    std::string feature;
    std::string present;
    std::string absent;
};

// Dialect alternatives `{alt0|alt1|…}`: the three delimiters and WHICH
// alternative this dialect is (AT&T is alternative 0 in gcc's and clang's x86
// ports). `declared == false` ⇒ braces and bars are ordinary template text.
struct AsmTemplateAlternatives {
    bool          declared  = false;
    std::string   open;
    std::string   separator;
    std::string   close;
    std::uint32_t select    = 0;
};

// Everything one dialect declares about template text forms.
//
// ⓘ `instanceNumber` (`%=`) is GNU C's and every measured port expands it, yet it
// is declared HERE, per dialect, beside the forms each dialect's own template
// surface supports: it is expanded before the dialect's lexer ever runs — it
// may sit INSIDE a label name (`.Lloop%=:`), so no token can carry it — and the
// one tier that expands text forms is the dialect-aware lowering. A dialect
// whose template surface lacks it declares none.
struct AsmTemplateTextForms {
    std::string                         instanceNumber;
    std::vector<AsmTemplateFixedForm>   fixed;
    std::vector<AsmTemplateFeatureForm> featureSelected;
    AsmTemplateAlternatives             alternatives;
};

// The three sigils the expansion must step AROUND rather than read — the
// placeholder that introduces every form, the escape that is a literal one and
// the opener of `%[name]` (the language's, `semantics.inlineAsmTemplateLexemes`)
// — plus this asm INSTANCE's number and the target's feature answer.
struct AsmTemplateTextFormContext {
    std::string_view placeholder;      // `%`
    std::string_view escape;           // `%%`  (passed through untouched)
    std::string_view symbolicNameOpen; // `[` of `%[name]` (a reference, passed through)
    std::uint64_t    instance = 0;     // this asm INSTANCE's number
    // Does the target declare `feature`? nullopt ⇒ the target does not KNOW the
    // feature at all, which is refused rather than read as "absent".
    std::function<std::optional<bool>(std::string_view feature)> targetHasFeature;
};

// Expand every declared form and the dialect alternatives in `text`, leaving
// operand references (`%0`, `%[name]`, `%l0`, `%w0`, …) and the escape exactly as
// written for the dialect's lexer. A `placeholder` followed by a character that
// no form, reference or escape can start — `%&`, or a declared-elsewhere form
// such as `%*` on aarch64 — is REFUSED by name, as are nested and unterminated
// alternatives: returns the message in the `unexpected` arm.
[[nodiscard]] DSS_EXPORT std::expected<std::string, std::string>
expandAsmTemplateTextForms(std::string_view text, AsmTemplateTextForms const& forms,
                           AsmTemplateTextFormContext const& ctx);

} // namespace dss
