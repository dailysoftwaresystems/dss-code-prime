/*
 * `struct timespec` — the per-format nanoseconds WIDTH, observed by STORE.
 *
 * ANCHOR: D-CSUBSET-C11-THREADS-TIMED.
 *
 * ★ WHY THIS EXAMPLE EXISTS. The shipped <time.h> declared `struct timespec`
 * FLAT — {i64 tv_sec, i64 tv_nsec} — on a descriptor available on pe, so every
 * Windows TU that spelled the tag received the LP64 body. On LLP64 the
 * nanoseconds field is `long`, which is FOUR bytes there, not eight.
 *
 * ★ WHY `sizeof` ALONE CANNOT SEE IT — the same-size trap `struct timeval`
 * already taught this project (D-FFI-MACHO-TIMEVAL-TV-USEC-WIDTH). The struct is
 * 16 bytes on BOTH sides: LP64 spends them on two 8-byte fields, LLP64 on an
 * 8-byte field, a 4-byte field and 4 bytes of trailing pad. A size check is
 * satisfied by the wrong answer, and so is an offset check — tv_nsec is at 8
 * either way. Only the WIDTH of the field moves.
 *
 * ★ SO THE INSTRUMENT IS THE STORE ITSELF, not a declaration.  The struct's
 * sixteen bytes are poisoned with a known pattern, ONE assignment is made to
 * `tv_nsec`, and the tail is read back through `unsigned char`:
 *
 *     LP64  — an 8-byte store: bytes 8..15 all become 0.
 *     LLP64 — a 4-byte store: bytes 8..11 become 0 and bytes 12..15 KEEP the
 *             poison, because they are padding the field does not own.
 *
 * A compiler that emits an 8-byte store through a 4-byte field wipes the poison
 * and this program exits non-zero. That is a real miscompile detector: it reads
 * the machine's behaviour, not the descriptor that produced it. A compile-only
 * pin cannot make this distinction at all, which is why it is here and not only
 * in tests/ffi.
 *
 * ★ THE ARM SPLIT IS `__LP64__`, the object format's OWN declaration
 * (D-PP-FORMAT-DATA-MODEL-PREDEFINES): the elf64 and macho64 format documents
 * declare it and the pe64 ones deliberately do not. The `#else` arm is therefore
 * the LLP64 world, and it states the STRONGER direction — that the tail survives
 * — which is exactly the assertion that fails if the flat LP64 body is ever
 * pasted back onto a Windows target.
 *
 * ★ `offsetof` IS NOT USED and could not be: <stddef.h> does not ship it
 * (D-FFI-OFFSETOF-MACRO). Offsets are taken with pointer arithmetic at run time,
 * which is the stronger measurement anyway.
 *
 * ✔MEASURED 2026-09-01, the two pe references probed SEPARATELY because they are
 * not one voice — a compile-only `_Static_assert` battery over sizeof / offsetof
 * / _Alignof plus `_Generic` for the exact C spelling, each reference also run
 * with a CONTROL arm carrying one deliberately-false assertion that failed to
 * compile on every one of them:
 *
 *   mingw-w64 gcc 13.2.0 (x86_64-w64-mingw32, ucrt)  {long long @0 (8), long @8 (4)}
 *   MSVC 19.51.36252                                 {long long @0 (8), long @8 (4)}
 *   WSL gcc 13.3.0 (glibc)                           {long      @0 (8), long @8 (8)}
 *   clang 18.1.3 (glibc)                             {long      @0 (8), long @8 (8)}
 *
 * Both pe references AGREE, so no disjunction question arises about the width.
 *
 * Exit code is 42 on every target by construction, so the status alone cannot
 * tell the worlds apart — the printed tag is what does.
 */
#include <stdio.h>
#include <time.h>

#define POISON 0xAB

int main(void) {
    struct timespec ts;
    unsigned char *raw = (unsigned char *)&ts;
    int i;
    int tailKeptPoison;
    int nsecOffset;
    int secOffset;

    /* sizeof agrees on both worlds; asserted so a REGRESSION in the total size
       is still caught, but it is deliberately NOT the discriminator. */
    if (sizeof(struct timespec) != 16u) {
        printf("FAIL sizeof=%d\n", (int)sizeof(struct timespec));
        return 1;
    }

    for (i = 0; i < 16; i++) raw[i] = POISON;

    secOffset  = (int)((unsigned char *)&ts.tv_sec  - raw);
    nsecOffset = (int)((unsigned char *)&ts.tv_nsec - raw);
    if (secOffset != 0 || nsecOffset != 8) {
        printf("FAIL offsets sec=%d nsec=%d\n", secOffset, nsecOffset);
        return 2;
    }

    /* THE MEASUREMENT: one store, then read the tail back as raw bytes. */
    ts.tv_nsec = 0;

    tailKeptPoison = (raw[12] == POISON && raw[13] == POISON &&
                      raw[14] == POISON && raw[15] == POISON);

    /* Bytes 8..11 belong to tv_nsec on BOTH worlds and must always be cleared —
       a positive control, so "tail kept poison" cannot pass by the store having
       been dropped entirely. */
    if (raw[8] != 0 || raw[9] != 0 || raw[10] != 0 || raw[11] != 0) {
        printf("FAIL nsec low half not stored\n");
        return 3;
    }

#ifdef __LP64__
    if (tailKeptPoison) {
        printf("FAIL LP64 tv_nsec stored only 4 bytes\n");
        return 4;
    }
    printf("timespec nsec=8 tail=cleared\n");
#else
    if (!tailKeptPoison) {
        printf("FAIL LLP64 tv_nsec stored 8 bytes over a 4-byte field\n");
        return 5;
    }
    printf("timespec nsec=4 tail=kept\n");
#endif

    /* tv_sec is 8 bytes on every current target — the field that does NOT move,
       so a change that "fixed" pe by shrinking the wrong member is caught too. */
    for (i = 0; i < 16; i++) raw[i] = POISON;
    ts.tv_sec = 0;
    for (i = 0; i < 8; i++) {
        if (raw[i] != 0) {
            printf("FAIL tv_sec stored fewer than 8 bytes at %d\n", i);
            return 6;
        }
    }
    if (raw[8] != POISON) {
        printf("FAIL tv_sec store ran past 8 bytes\n");
        return 7;
    }

    return 42;
}
