/* TF-C96 D-CSUBSET-DARWIN-BSD-STRUCT-BY-NAME witness: `struct itimerval` shipped by the <sys/time.h> descriptor as TWO BY-VALUE `struct timeval` members.
 * A descriptor SHADOWS the real header text, so a name the descriptor omits is
 * simply MISSING from every TU that includes it: `struct itimerval` (SDK
 * `sys/time.h:92`) was absent, xnu's `struct extern_proc` embeds one BY VALUE, so
 * extern_proc stayed incomplete and made `kinfo_proc`'s `kp_proc` a second
 * incomplete member — 4 `S0026` in sqlite `test1.c` + `mem1.c` from that ONE hole.
 * The shape is `{ struct timeval it_interval; struct timeval it_value; }`: nesting
 * by NAME, by VALUE — not a pointer, not a re-spelling of the inner fields. By
 * name is what lets it need no per-format `variants` of its own: the reference
 * resolves to whichever timeval variant this target selected, so the one place a
 * tv_usec width is stated stays the timeval entry.
 *
 * WHY THE LAYOUT IS ASSERTED AT RUNTIME AND NOT ASSUMED — two different risks,
 * each with its own pin, because neither pin can see the other's failure.
 *   (1) THE OUTER SHAPE is DERIVED, not restated: the descriptor spells both
 *       members as the bare tag `timeval`, so the outer offsets come out of the
 *       by-name resolution + the layout engine. Drop `it_interval`, or re-spell
 *       either member as a POINTER, and the offsets MOVE — which is what the
 *       pointer-arithmetic offsets below catch.
 *   (2) THE INNER WIDTH is INVISIBLE to (1). Darwin's timeval is 16 bytes — the
 *       SAME sizeof as glibc's {i64,i64} — but its tv_usec is
 *       __darwin_suseconds_t = __int32_t, not glibc's long (the trap
 *       `D-FFI-MACHO-TIMEVAL-TV-USEC-WIDTH` was minted for). A by-value nest
 *       INHERITS that mistake in perfect silence: an inner timeval carrying an
 *       i64 tv_usec still totals 16, so the outer sizeof is still 32 and BOTH
 *       member offsets are still 0 and 16. NO offset anywhere moves. What breaks
 *       is the VALUE — a read folds 4 undefined padding bytes into tv_usec's high
 *       half, a write clobbers them — and it breaks in every by-value nest of
 *       this struct: xnu `sys/proc.h:122` embeds one as `struct extern_proc`'s
 *       `p_realtimer` (with `p_rtime` a bare timeval at `:123`), and
 *       `sys/sysctl.h:473` embeds THAT as `kinfo_proc`'s `kp_proc`. Hence the
 *       INNER pins: `sizeof(tv_usec) == 4` plus a byte-level STORE witness.
 *
 * MEASURED NATIVELY (Apple clang, arm64 Darwin, macOS SDK):
 *     sizeof(struct itimerval) == 32 ; it_interval @ 0 ; it_value @ 16
 *     sizeof(struct timeval)   == 16 ; tv_sec 8 bytes @ 0 ; tv_usec 4 bytes @ 8
 * Every one of those is re-derived HERE at runtime — `sizeof` on the objects, and
 * offsets by pointer arithmetic on a REAL `struct itimerval` — rather than
 * restated as a constant.
 *
 * NO `setitimer`/`getitimer` CALL: the descriptor ships the TYPE and no such
 * symbol, and inventing a call would test the loader instead of the layout.
 * Instead both nested timevals are genuinely USED — written through the nested
 * member path and read back — so the inner struct is materialized, not merely
 * named.
 *
 * EXIT, built from measured facts:
 *     it_value's offset (16) + tv_usec's offset inside it (8)
 *   + sizeof(struct timeval) (16)                                 = 40
 *   + it_value.tv_sec read back after a runtime store of g_secs    =  2
 *   ----------------------------------------------------------------- 42
 * A one-member (or pointer-member) itimerval moves it_value's offset; a wrong
 * inner width moves tv_usec's offset and trips the poison witness; a missing
 * descriptor row does not compile at all.
 *
 * RED-ON-DISABLE: delete the `itimerval` row from the shipped <sys/time.h>
 * descriptor -> the struct is not injected and `test1.c`'s original `S0026`
 * incomplete-type failure returns -> no binary. Re-spell either member as a
 * POINTER, or drop `it_interval`, and the offset guards return 6. Regress the
 * inner tv_usec to i64 and the width guard returns 4 while the padding-survival
 * witness returns 9.
 *
 * Single arm64-macho target -> the macos-latest CI leg (native Apple Silicon);
 * the release arm runs the optimizer over the folded sizes, the byte stores and
 * the nested member accesses.
 */
#include <sys/time.h>

/* A mutable global = a runtime-opaque operand for the read-back term. */
int g_secs = 2;

int main(void) {
    struct itimerval t;
    char *p = (char *)&t;
    int offInterval;
    int offValue;
    int offSec;
    int offUsec;
    int total;

    /* (a) the SIZE pins — outer geometry AND the inner field widths, because the
    ** outer sizes alone cannot discriminate a 16-byte timeval from a 16-byte one. */
    if (sizeof(struct itimerval) != 32) return 1;
    if (sizeof(struct timeval)   != 16) return 2;
    if (sizeof(t.it_value.tv_sec)  != 8) return 3;   /* __darwin_time_t = long   */
    if (sizeof(t.it_value.tv_usec) != 4) return 4;   /* __int32_t (elf: 8)       */

    /* (b) the OFFSETS, from pointer arithmetic on a REAL object — the two
    ** timevals are BY VALUE and adjacent, and tv_usec sits 8 into each. */
    offInterval = (int)((char *)&t.it_interval - (char *)&t);
    offValue    = (int)((char *)&t.it_value    - (char *)&t);
    offSec      = (int)((char *)&t.it_value.tv_sec  - (char *)&t.it_value);
    offUsec     = (int)((char *)&t.it_value.tv_usec - (char *)&t.it_value);
    if (offInterval != 0)  return 5;
    if (offValue    != 16) return 6;
    if (offSec      != 0)  return 7;
    if (offUsec     != 8)  return 8;

    /* (c) the STORE-WIDTH witness for the INNER member (the same device
    ** shipped_timeval_macho uses, now one nest down). it_value occupies bytes
    ** 16..31; tv_sec is 16..23, tv_usec 24..27, and 28..31 is the inner struct's
    ** trailing pad. An i32 store to tv_usec leaves the poison intact; a stale
    ** i64 store writes 24..31 and erases it. */
    p[28] = 1; p[29] = 2; p[30] = 3; p[31] = 4;
    t.it_value.tv_usec = 250000;
    if (p[28] != 1 || p[29] != 2 || p[30] != 3 || p[31] != 4) return 9;
    if (t.it_value.tv_usec != 250000) return 10;

    /* (d) write/read through BOTH nested timevals, so each is really used. */
    t.it_interval.tv_sec  = 0;
    t.it_interval.tv_usec = 0;
    t.it_value.tv_sec     = g_secs;
    if (t.it_interval.tv_sec != 0 || t.it_interval.tv_usec != 0) return 11;

    total = offValue + offUsec + (int)sizeof(struct timeval);  /* 16 + 8 + 16 = 40 */
    total = total + (int)t.it_value.tv_sec;                    /* + g_secs (2) = 42 */
    return total;
}
