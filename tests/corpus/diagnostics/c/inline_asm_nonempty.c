/* inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS): a `%` PLACEHOLDER FORM inside a BASIC
 * template — one whose statement opens no operand section at all — is refused with
 * exactly ONE S_InlineAsmPlaceholderInBasicTemplate (S006B).
 *
 * ★★★ WHY THIS FILE NO LONGER PINS S0057. It used to be `__asm__("nop")` pinning
 * S_InlineAsmNonEmptyTemplate — "a non-empty template carries real per-target
 * instructions cycle-1 cannot emit". P5 emits them: `__asm__("nop")` now compiles and
 * runs, so that golden could not survive. The protection it existed for — a template's
 * contents must never be silently mis-emitted — moves here, to the one template
 * question a basic statement can still get wrong.
 *
 * ★★★ THE DISCRIMINATOR IS MEASURED, NOT ASSUMED, AND IT IS WHY THIS CODE EXISTS.
 * ✔MEASURED 2026-08-14 on gcc 13.3.0 AND clang 18.1.3 (sources fed as base64 so no
 * shell quoting could confound the result): in a BASIC template `%` is LITERAL —
 * `__asm__("xorl %eax, %eax")` emits `xorl %eax, %eax` unchanged — while the SAME text
 * in an EXTENDED template is an ERROR on both ("operand number missing after %-letter"
 * / "invalid % escape"). ANY colon makes a statement extended. So the two surfaces lex
 * DIFFERENTLY, and a `%0` here has no operand list to bind against.
 *
 * ⚠ THIS FILE DELIBERATELY DOES NOT USE `%eax`. That spelling is LEGAL in a basic
 * template and must stay legal — refusing it would be a divergence from both reference
 * compilers, which is the bidirectional half of the standing rule. The subject is `%0`,
 * a PLACEHOLDER form, which is the shape that cannot mean anything here.
 * ⚠ THE DANGEROUS IMPLEMENTATION IS THE TEMPTING ONE, and this pin is aimed at it: an
 * expander that unescapes `%%`->`%` into a buffer and then lexes placeholders would
 * silently bind `%%0` to operand 0 — a miscompile gcc does NOT have (it emits `A%0B`
 * from `"A%%0B"`, the emitted `%0` deliberately not re-read).
 *
 * RED-on-disable: remove the basic-vs-extended discriminator -> `%0` is treated as a
 * placeholder, binds against an EMPTY operand list, and this either compiles clean
 * (golden goes EMPTY, which the harness refuses) or reports S006A instead — a
 * different code, so the golden moves either way and names which half broke. */
int main(void) { __asm__("nop %0"); return 0; }
