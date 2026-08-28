// D-PP-DEFINED-VIA-MACRO-EXPANSION -- the runtime witness for the whole
// conditional-inclusion OPERATOR FAMILY reached VIA MACRO EXPANSION, and for the
// OPERAND BARRIER that decides, per operator, what expansion may touch.
//
// C 6.10.1 leaves the `defined` case UNDEFINED, so the standard is NOT the
// argument. The argument is the union of the references, MEASURED 2026-08-27:
// gcc 13.3.0 (-std=c2x -pedantic), clang 18.1.3 (-std=c23 -pedantic) and MSVC
// 19.51 (/std:c17 /W4, traditional AND /Zc:preprocessor) ALL accept and evaluate
// every shape below, with identical answers. gcc and clang additionally warn
// (-Wexpansion-to-defined) on the `defined` shapes, MSVC as C5105; DSS emits
// P_PreprocessorDefinedFromExpansion for the same four, at Warning severity, so
// this example compiles with warnings and no errors -- which is exactly what all
// three references do with it.
//
// Apple's own SDK depends on the third shape: `secure/_string.h`'s
// `__is_modern_darwin` is a function-like macro whose last operand is literally
// `defined(__DRIVERKIT_VERSION_MIN_REQUIRED)`, and the row was opened because
// that header is reached by the corpus.
//
// ★★ SEVEN INDEPENDENT SHAPES, AND THE DELTAS ARE POWERS OF TWO. Every subset of
// {64,32,16,8,4,2,1} sums to a DISTINCT value, so the exit code does not merely
// say "something is wrong" -- it names EXACTLY which shapes failed, and no two
// failures can cancel or masquerade as a third.
//
//   (1) BASE      40, else 104  (+64) -- THE OPERAND BARRIER, and the shape that
//       separates a correct fix from a plausible one: ZERO is defined TO 0. With
//       the operand protected, `defined(ZERO)` is 1. With it expanded (the
//       defect), it is `defined(0)` -- a DIFFERENT QUESTION, not a syntax error,
//       which is why only this shape catches that particular wrong fix.
//   (2) MARGIN     2, else  34  (+32) -- NEGATIVE polarity, so an implementation
//       that hard-wires "true" is caught.
//   (3) BONUS      0, else  16  -- the real Apple-SDK function-like shape.
//   (4) TAX        0, else   8  -- the `defined` KEYWORD ITSELF produced by a
//       macro, operand written at the call site. Keyword and operand come from
//       two different constructs, so only a barrier driven over the expander's
//       OUTPUT joins them.
//   (5) FEE        0, else   4  -- `__has_include` produced by a macro, quote
//       form, resolving a REAL neighbouring header. Before the operator fold
//       moved past expansion this was `error[P0013]: trailing tokens after #if
//       controlling expression` and no binary was produced at all.
//   (6) LEVY       0, else   2  -- the RE-EXAMINE arm: an operand matching
//       NEITHER delimited form IS macro-expanded and re-examined (C's own
//       `#include MACRO` rule). Protecting it -- the naive "treat every operand
//       like `defined`" fix -- answers 0 where all three references answer 1.
//   (7) DUTY       0, else   1  -- the `__has_include` KEYWORD from a macro.
//
// 40 + 2 + 0 + 0 + 0 + 0 + 0 = 42.
//
// FOLD RESISTANCE: all seven values reach `add7` as FUNCTION ARGUMENTS, so the
// baseline (unoptimized) arm keeps a live runtime add rather than folding to one
// immediate at exit; the `release` arm runs the shipped pipeline over the same
// source. The PRIMARY witness is still compile-time -- before the fix, eleven of
// these shapes were hard refusals and NO binary was produced.

#define FOO  1
#define ZERO 0

// (1) THE OPERAND BARRIER. `defined(ZERO)` must ask "is ZERO defined?" (yes),
// never "is 0 defined?" -- answering the second at all is the silent change of
// question this example exists to catch.
#define HAS_ZERO defined(ZERO)
#if HAS_ZERO
#define BASE 40
#else
#define BASE 104
#endif

// (2) NEGATIVE polarity: the name really is undefined, so the operator must
// answer 0 and the #else must win.
#define HAS_NEVER defined(NEVER_DEFINED_ANYWHERE)
#if HAS_NEVER
#define MARGIN 34
#else
#define MARGIN 2
#endif

// (3) THE SHIPPED-SDK SHAPE (Apple `secure/_string.h`, `__is_modern_darwin`): a
// function-like macro whose LAST operand is a `defined(...)` and whose earlier
// operands are ordinary comparisons over the parameters.
// LO(101000) >= 999999 is false; HI(130000) >= 120000 is TRUE.
#define LO 101000
#define HI 130000
#define IS_MODERN(ios, macos) \
    (LO >= (macos) || HI >= (ios) || defined(NOT_SET))
#if IS_MODERN(120000, 999999)
#define BONUS 0
#else
#define BONUS 16
#endif

// (4) The `defined` KEYWORD from a macro, operand written at the call site.
#define D defined
#if D(FOO)
#define TAX 0
#else
#define TAX 8
#endif

// (5) `__has_include` produced by a macro, QUOTE form, resolving the real
// neighbouring header. Quote form on purpose: an angle probe resolves shipped
// descriptors whose availability is PER-OBJECT-FORMAT, and this example declares
// four targets that must all reach the same exit code.
#define HAS_LOCAL __has_include("pp_local.h")
#if HAS_LOCAL
#define FEE 0
#else
#define FEE 4
#endif

// (6) THE RE-EXAMINE ARM, and it pulls the OPPOSITE way from (1): an operand
// matching neither delimited form MUST be expanded, then re-examined. Protecting
// it would leave a bare identifier where a header name belongs and refuse.
#define LOCAL_NAME "pp_local.h"
#if __has_include(LOCAL_NAME)
#define LEVY 0
#else
#define LEVY 2
#endif

// (7) The `__has_include` KEYWORD from a macro, operand at the call site.
#define HI_OP __has_include
#if HI_OP("pp_local.h")
#define DUTY 0
#else
#define DUTY 1
#endif

int add7(int a, int b, int c, int d, int e, int f, int g) {
    return a + b + c + d + e + f + g;
}

int main(void) {
    return add7(BASE, MARGIN, BONUS, TAX, FEE, LEVY, DUTY);
}
