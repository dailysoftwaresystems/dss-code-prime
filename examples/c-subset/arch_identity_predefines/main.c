/*
 * TF-C74 (per-ARCHITECTURE identity predefined macros) — the runnable witness.
 *
 * `src/dss-config/targets/<arch>.target.json` now carries a `predefinedMacros`
 * root key whose rows are merged with the language config's
 * `preprocess.predefinedMacros`, format-filtered ONCE, and fail loud on a
 * name collision. This example is the end-to-end pin: it drives the merged
 * table through preprocess -> parse -> lower -> codegen -> LINK -> spawn ->
 * exit code, on every shipped (architecture x object-format) leg.
 *
 * ★ NON-VACUITY. Four INDEPENDENTLY FALSIFIABLE compile-time legs, each red on
 * its own (the observed red-on-disable matrix is recorded in expected.json's
 * $comment), plus a runtime arm that proves EXECUTION rather than selection:
 *
 *   LEG 1  EXHAUSTIVENESS   — every shipped target must predefine an identity
 *                             macro. Red when the feature is off entirely
 *                             (`predefinedMacros` absent / unread).
 *   LEG 2  MUTUAL EXCLUSION — exactly ONE architecture's list may reach a given
 *                             translation unit. Red if the merge UNIONS every
 *                             target's rows instead of the active target's.
 *   LEG 3  PLATFORM GATE    — BOTH directions of `availableObjectFormats`.
 *                             `__arm64__`/`__arm64` are APPLE-ONLY spellings:
 *                             present on arm64-apple-darwin, ABSENT on
 *                             aarch64-linux-gnu (MEASURED — see expected.json).
 *                             Red if the gate leaks onto ELF, and red if it
 *                             fails to fire on Mach-O.
 *   LEG 4  VALUE            — the `value` field, not merely definedness. Red if
 *                             the row is seeded as a bare `#define NAME`.
 *
 * The macro names below are the C-source side of a config fact; the COMPILER
 * never hardcodes them (the merge compares config strings to config strings).
 *
 * ★ THE PER-TARGET STDOUT TAG closes a vacuity the four legs CANNOT: every
 * route is arithmetically compensated to exit 42, so a target that is handed
 * the WRONG architecture's identity list still exits 42 (MEASURED: patch
 * arm64.target.json's `predefinedMacros` to x86_64's rows and every leg stays
 * green). The routes therefore also `puts` a distinct tag, pinned per target in
 * expected.json, so the three routes are told apart by the RUNNER and not only
 * by arithmetic that happens to agree.
 *
 * ★ DELIBERATELY NOT USED as the runtime arch cross-check: bare-`char`
 * signedness. `arm64.target.json` declares `charIsUnsigned: true` for BOTH
 * arm64 platforms, but clang defines `__CHAR_UNSIGNED__` only for
 * aarch64-linux-gnu — NOT for arm64-apple-darwin (MEASURED 2026-07-28).
 * Building the exit code on it would bake that known divergence into a green
 * test AND would make the clang ground-truth build disagree with DSS on this
 * very host. The identity-macro VALUES carry the arithmetic instead.
 */

#include <stdio.h>   /* `puts` — the shipped descriptor, resolved per format */

/* ── LEG 1: EXHAUSTIVENESS ────────────────────────────────────────────────
 * Every architecture DSS ships predefines an identity macro. If the target's
 * `predefinedMacros` key is never read, NEITHER name exists and this fires on
 * EVERY leg — the whole-feature-off tripwire. */
#if !defined(__aarch64__) && !defined(__x86_64__)
#error "TF-C74 leg 1: no architecture identity macro is predefined -- the target's predefinedMacros are not reaching the preprocessor"
#endif

/* ── LEG 2: MUTUAL EXCLUSION ──────────────────────────────────────────────
 * A translation unit is compiled for ONE architecture, so at most one
 * architecture's rows may be in scope. This fires if the merge unions every
 * shipped target's list instead of selecting the ACTIVE target's. (A second,
 * independent net rides along below: both branches define `archIdentitySum`,
 * so a union would ALSO be a duplicate-definition error even with `#error`
 * regressed.) */
#if defined(__aarch64__) && defined(__x86_64__)
#error "TF-C74 leg 2: __aarch64__ and __x86_64__ are BOTH predefined -- the merge is unioning every target's macros instead of the active target's"
#endif

#if defined(__aarch64__)

/* ── LEG 3a: PLATFORM GATE, APPLE DIRECTION ───────────────────────────────
 * On an Apple (Mach-O) AArch64 target the Apple-only spellings MUST be
 * present: the macOS SDK's own arch ladders (machine/_types.h, sys/cdefs.h)
 * gate on `__arm64__`, not on `__aarch64__`. Fires if `availableObjectFormats`
 * wrongly EXCLUDES macho (e.g. the gate is inverted to ["elf"], or the gated
 * rows are dropped from the effective list on their own format). */
#  if defined(__APPLE__) && !defined(__arm64__)
#    error "TF-C74 leg 3a: __APPLE__ is predefined but __arm64__ is not -- the macho-gated Apple spellings are missing on their OWN format"
#  endif

/* ── LEG 3b: PLATFORM GATE, NON-APPLE DIRECTION ───────────────────────────
 * On a non-Apple AArch64 target the Apple-only spellings MUST be absent —
 * aarch64-linux-gnu does not define them (MEASURED). Fires if
 * `availableObjectFormats` is ignored on target rows and the Apple spelling
 * leaks onto the ELF leg. */
#  if !defined(__APPLE__) && defined(__arm64__)
#    error "TF-C74 leg 3b: __arm64__ is predefined without __APPLE__ -- an Apple-only spelling leaked onto a non-Apple format; availableObjectFormats is not honoured on target rows"
#  endif

/* ── LEG 4: VALUE, NOT JUST DEFINEDNESS ───────────────────────────────────
 * Both ungated AArch64 rows carry `"value": "1"`. Fires if the merge seeds
 * definedness only (a bare `#define NAME`, which makes the controlling
 * expression a syntax error) or materializes any other spelling. */
#  if __aarch64__ != 1 || __ARM_ARCH_ISA_A64 != 1
#    error "TF-C74 leg 4: an AArch64 identity macro does not carry the value 1 -- the row's `value` field is dropped or wrong"
#  endif

#  if defined(__arm64__)
/* Apple AArch64: all four spellings, each == 1 (leg 4 for the gated pair). */
#    if __arm64__ != 1 || __arm64 != 1
#      error "TF-C74 leg 4 (gated rows): an Apple AArch64 spelling does not carry the value 1"
#    endif
#    define DSS_ARCH_SPELLINGS (__aarch64__ + __ARM_ARCH_ISA_A64 + __arm64__ + __arm64)
#    define DSS_ARCH_SCALE     6      /* 4 spellings * 6  == 24 */
#    define DSS_ARCH_TAG       "arch=aarch64-apple"
#  else
/* Non-Apple AArch64: the two UNGATED spellings only. */
#    define DSS_ARCH_SPELLINGS (__aarch64__ + __ARM_ARCH_ISA_A64)
#    define DSS_ARCH_SCALE     12     /* 2 spellings * 12 == 24 */
#    define DSS_ARCH_TAG       "arch=aarch64"
#  endif

/* The arch-selected function (AArch64 definition). Its RETURN VALUE is summed
 * from the identity macros' VALUES, so leg 4 is re-proved at RUNTIME: a wrong
 * value, a dropped gated row, or a leaked gated row changes the sum and the
 * program exits something other than 42 even if every `#error` were regressed. */
int archIdentitySum(void) {
    return DSS_ARCH_SPELLINGS;
}

#endif  /* __aarch64__ */

#if defined(__x86_64__)

/* ── LEG 4 (x86_64 mirror): VALUE, NOT JUST DEFINEDNESS ───────────────────
 * All four x86_64 spellings are UNGATED — MEASURED identical on
 * x86_64-linux-gnu, x86_64-apple-darwin AND x86_64-pc-windows-msvc — and each
 * carries `"value": "1"`. */
#  if __x86_64__ != 1 || __x86_64 != 1 || __amd64__ != 1 || __amd64 != 1
#    error "TF-C74 leg 4 (x86_64): an x86_64 identity macro does not carry the value 1 -- the row's `value` field is dropped or wrong"
#  endif

#  define DSS_ARCH_SPELLINGS (__x86_64__ + __x86_64 + __amd64__ + __amd64)
#  define DSS_ARCH_SCALE     6        /* 4 spellings * 6  == 24 */
#  define DSS_ARCH_TAG       "arch=x86_64"

/* The arch-selected function (x86_64 definition). */
int archIdentitySum(void) {
    return DSS_ARCH_SPELLINGS;
}

#endif  /* __x86_64__ */

/* Fold-resistance: the three operands cross a real call boundary (the
 * `preprocessor_error_dead_branch` precedent), so the baseline arm keeps a live
 * runtime multiply-add rather than folding to one immediate at exit. */
int combine(int spellings, int scale, int base) {
    return spellings * scale + base;
}

int main(void) {
    /* The route's own name, pinned per target in expected.json — the ONE
     * observable that tells the three routes apart (the exit code cannot: every
     * route is compensated to 42 on purpose, so all four legs share one
     * `exitCode`). */
    puts(DSS_ARCH_TAG);

    /* A `volatile` seed keeps `archIdentitySum()`'s result a RUNTIME value, so
     * the `release` arm exercises real machine arithmetic over the merged
     * macro table rather than a const-fold (the `char_signedness` precedent). */
    volatile int live = 1;
    int spellings = archIdentitySum() * (int)live;
    return combine(spellings, DSS_ARCH_SCALE, 18);   /* 4*6+18 == 2*12+18 == 42 */
}
