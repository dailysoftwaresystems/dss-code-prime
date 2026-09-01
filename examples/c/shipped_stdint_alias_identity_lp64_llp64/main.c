/*
 * The ELF and PE halves of the <stdint.h> alias identity table — the CONTROL.
 *
 * ANCHOR: D-FFI-DESCRIPTOR-TYPE-ALIAS-SPELLING-KEYED-ON-DATA-MODEL-ALONE.
 *
 * ★ WHY A CONTROL EXAMPLE EXISTS AT ALL, AND WHY IT IS NOT OPTIONAL. That row
 * added a Darwin arm to selectors that previously keyed on the data model alone.
 * glibc's LP64 `uint64_t` genuinely IS `unsigned long`, and the LLP64 answers were
 * already right, so the change had to be strictly ADDITIVE: a fix that "corrected"
 * Darwin by moving ELF or PE would be worse than the defect it removed. This
 * program is the runnable statement that neither moved. Its Darwin sibling is
 * `examples/c/shipped_stdint_alias_identity_macho`, and the two are deliberately
 * separate manifests rather than one: nothing a C program can read distinguishes
 * elf-LP64 from macho-LP64 (both formats predefine `__LP64__`/`_LP64` and neither
 * predefines anything else that separates them), so one source would have had to
 * infer its own platform from another config fact — which would make the pin
 * partly self-referential. Splitting by MANIFEST puts the platform in the target
 * spec, where it is a fact and not an inference.
 *
 * ★ WHAT IS ASSERTED, AND WITH WHICH INSTRUMENT. `sizeof` cannot see this defect:
 * every alias here is 8 bytes on all three platforms, before and after. The two
 * discriminators that can are pointer-assignment compatibility and `_Generic`, and
 * both are used:
 *
 *   COMPILE TIME — each initializer stores the address of an alias into a pointer
 *                  to an INDEPENDENTLY SPELLED named C type. C requires compatible
 *                  pointees, so the file compiles only if every alias IS the type
 *                  named beside it. Wrong spelling ⇒ `S_TypeMismatch`, no artifact.
 *   RUN TIME     — `_Generic` re-derives each spelling as a string; the manifest
 *                  pins a DIFFERENT `expectedStdout` per target, so the elf and pe
 *                  answers cannot be satisfied by one blanket result.
 *
 * ★ THE ARM SPLIT IS `__LP64__`, WHICH IS THE OBJECT FORMAT'S OWN DECLARATION
 * (D-PP-FORMAT-DATA-MODEL-PREDEFINES): the elf64 and macho64 format documents
 * declare it, the pe64 ones deliberately do not. So the `#else` arm here is the
 * LLP64 world and states the STRONGER direction — that `size_t` is
 * `unsigned long long` and NOT `unsigned long`, which is precisely the assertion
 * that fails if someone pastes the LP64 rows onto a Windows target.
 *
 * ✔MEASURED 2026-09-01, references probed SEPARATELY (they are not one voice):
 *   elf   LP64 : WSL gcc 13.3.0, clang 18.1.3, aarch64-linux-gnu-gcc 13.3.0 —
 *                every alias `long`/`unsigned long`.
 *   pe    LLP64: mingw-w64 gcc 13.2.0 (ucrt) and MSVC cl.exe 19.51.36252 —
 *                every alias `long long`/`unsigned long long`. The two agree.
 * Both were read with a compile-only `_Generic` probe that tested each alias
 * against every candidate named type, so the answer is the compiler's after the
 * whole typedef chain rather than a grep of headers.
 *
 * FOLD-RESISTANCE: the values arrive through `volatile` seeds read back through
 * the identity pointers, so the exit arithmetic stays live rather than folding to
 * one immediate, and the manifest's `release` arm runs the shipped release
 * pipeline over the same source.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* 8 + 9 + 10 + 7 + 8 == 42 on BOTH arms — the exit code deliberately cannot tell
 * the two worlds apart, so it is the printed tag, not the status, that proves the
 * right arm was taken. The status still catches a wrong VALUE crossing the
 * pointers. */
static volatile unsigned long long gSeedU64  = 8;
static volatile unsigned long long gSeedUmax = 9;
static volatile unsigned long long gSeedSize = 10;
static volatile long long          gSeedI64  = 7;
static volatile long long          gSeedImax = 8;

static uint64_t  gU64;
static uintmax_t gUmax;
static size_t    gSize;
static int64_t   gI64;
static intmax_t  gImax;

#ifdef __LP64__

/* ── ELF / LP64 — glibc spells every one of them `long` ─────────────────── */
static unsigned long *const kU64  = &gU64;
static unsigned long *const kUmax = &gUmax;
static unsigned long *const kSize = &gSize;
static long          *const kI64  = &gI64;
static long          *const kImax = &gImax;

#else

/* ── PE / LLP64 — the NEGATIVE arm, and the stronger one. `unsigned long` is
 * 32 bits here, so naming `unsigned long long` is a statement that these aliases
 * did NOT take the LP64 spelling. ─────────────────────────────────────────── */
static unsigned long long *const kU64  = &gU64;
static unsigned long long *const kUmax = &gUmax;
static unsigned long long *const kSize = &gSize;
static long long          *const kI64  = &gI64;
static long long          *const kImax = &gImax;

#endif

#define SPELL(T) _Generic((T)0,                 \
    unsigned long:      "ul",                   \
    unsigned long long: "ull",                  \
    long:               "l",                    \
    long long:          "ll",                   \
    default:            "?")

int main(void) {
    *kU64  = (unsigned long long)gSeedU64;
    *kUmax = (unsigned long long)gSeedUmax;
    *kSize = (unsigned long long)gSeedSize;
    *kI64  = (long long)gSeedI64;
    *kImax = (long long)gSeedImax;

    printf("u64=%s umax=%s uptr=%s size=%s i64=%s imax=%s\n",
           SPELL(uint64_t), SPELL(uintmax_t), SPELL(uintptr_t),
           SPELL(size_t), SPELL(int64_t), SPELL(intmax_t));

    return (int)((unsigned long long)*kU64 + (unsigned long long)*kUmax
                 + (unsigned long long)*kSize + (unsigned long long)*kI64
                 + (unsigned long long)*kImax);
}
