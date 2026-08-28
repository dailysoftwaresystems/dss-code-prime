/* D-CONFIG-SETJMP-MACRO-INVISIBLE-ON-ELF: `setjmp` must be MACRO-VISIBLE on the
   formats whose own reference toolchain makes it a macro — and must NOT be on
   the one whose reference does not.

   ★★ THIS EXAMPLE ENCODES THE CONTRACT, NOT ONE FORMAT'S ANSWER, and that is
   the whole design. The references DISAGREE WITH EACH OTHER, per target:

     elf   glibc's <setjmp.h> carries `#define setjmp(env) _setjmp (env)`
           ✔MEASURED 2026-08-27: `#ifndef setjmp -> #error` is rc=0 under
           gcc 13.3.0 -std=c2x AND clang 18.1.3 -std=c23, at c99/c11/c2x/gnu2x
           alike, with a control #error that fired on both instruments.
     pe    MSVC's <setjmp.h> does `#define setjmp _setjmp`; DSS has always
           shipped the pe arm (2-arg `_setjmp(env, 0)` -> ucrtbase
           `__intrinsic_setjmp`).
     macho Apple's SDK <setjmp.h> declares `extern int setjmp(jmp_buf);` and
           carries NO #define at all. ✔MEASURED on the operator's macOS host
           (MacOSX.sdk/usr/include/setjmp.h, clang -std=c2x, same two controls):
           the probe is rc=1 there.

   ⚠ ISO DOES NOT SETTLE THIS AND THE ROW DOES NOT REST ON IT. C23 §7.13p1 lists
   `setjmp` among the macros the header defines, but §7.13p4 says outright that
   "It is unspecified whether setjmp is a macro or an identifier declared with
   external linkage". The binding rule is the REFERENCE UNION, applied per
   TARGET — which is why the right answer here is genuinely different on Mach-O,
   and why sweeping a `macho` arm in "for consistency" would be a DEFECT, not a
   tidy-up: it would put DSS above the only reference that target has.

   ⚠ BEFORE (✔MEASURED through the shipped CLI, one binary, one source, against
   a config root pinned outside the repo so no sibling lane's edit could move
   it, with a positive control proving that snapshot is what loaded):
     elf64-x86_64 rc=1 P001E · elf64-aarch64 rc=1 P001E · pe64 rc=0
     · macho64-x86_64 rc=1 · macho64-arm64 rc=1
   AFTER: elf64-x86_64 rc=0 · elf64-aarch64 rc=0 · pe64 rc=0
     · macho64-x86_64 rc=1 · macho64-arm64 rc=1   <- macho UNCHANGED, deliberately.

   ★ THE EXIT CODE NAMES THE DIRECTION OF A REGRESSION, not merely its
   existence, because the two directions have opposite fixes:
     42 = contract satisfied on this format
     34 = REQUIRED BUT ABSENT   -> a variant arm was dropped (conformance gap)
     36 = FORBIDDEN BUT PRESENT -> an arm was swept onto a format whose own
                                   reference has none (invented extension)

   ★ AND IT RUNS THE EXPANSION, which `#ifdef` alone cannot witness. On elf the
   replacement is SELF-REFERENTIAL (`setjmp(env)` -> `setjmp(env)`), so the
   preprocessor's hideset is what stops it recursing; on pe it rewrites to a
   DIFFERENT 2-arg entry point. The round trip below therefore proves the macro
   is usable, not merely defined — and that the elf arm still binds glibc's
   sigmask-saving `setjmp`, which its `longjmp` pairing and the 200B jmp_buf
   depend on.

   ⚠ THIS EXAMPLE IS NOT SUFFICIENT ON ITS OWN AND ITS SIBLING PIN IS NOT
   OPTIONAL. An exit code is observable only where the artifact RUNS, and the
   elf/macho arms below declare `runOn: ["linux"]` / `["darwin"]` — so on a
   Windows host every arm but pe is BUILD-ONLY and this file cannot fail for the
   elf regression it exists to catch. `tests/program/test_setjmp_macro_visibility.cpp`
   asserts the same contract at COMPILE time, for all three formats, on every
   host. (Lane T measured the vacuous-pin version of exactly this mistake in the
   sibling stdio row: deleting an `elf` variant left its example GREEN on Windows.) */
#include <setjmp.h>

/* What this TARGET's own reference toolchain does. Format-keyed, never
   host-keyed: `__APPLE__` is predefined by the c language schema for the macho
   object format, so a Windows host cross-building macho takes the macho arm. */
#if defined(__APPLE__)
#  define DSS_SETJMP_MUST_BE_A_MACRO 0
#else
#  define DSS_SETJMP_MUST_BE_A_MACRO 1
#endif

/* What DSS actually did. */
#ifdef setjmp
#  define DSS_SETJMP_IS_A_MACRO 1
#else
#  define DSS_SETJMP_IS_A_MACRO 0
#endif

static int visibilityScore(void) {
    if (DSS_SETJMP_IS_A_MACRO == DSS_SETJMP_MUST_BE_A_MACRO) return 20;
    return DSS_SETJMP_IS_A_MACRO ? 14 : 12;
}

static jmp_buf g_env;

/* Mutable globals so no pass can const-fold the round trip away — the
   anti-fold idiom the sibling setjmp_longjmp example established. */
static volatile int g_bump = 22;

int main(void) {
    /* `volatile` because a local modified between setjmp and longjmp is
       indeterminate after the resume otherwise (C23 §7.13.2.1p3). */
    volatile int carried = 0;

    /* The invocation shape is one ISO §7.13.1.1p4 permits: an operand of an
       equality operator whose other operand is an integer constant expression,
       the whole being the controlling expression of a selection statement. */
    if (setjmp(g_env) == 0) {
        carried = g_bump;
        longjmp(g_env, 1);
    }

    return visibilityScore() + carried;   /* 20 + 22 = 42 */
}
