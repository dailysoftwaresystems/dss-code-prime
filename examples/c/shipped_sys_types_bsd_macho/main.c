/* TF-C90 (D-CSUBSET-SYS-TYPES-BSD-SPELLING-GROUP-ABSENT) — the BSD-ONLY half of
 * the shipped <sys/types.h> spelling group: `fixpt_t` and `segsz_t`, which are
 * macho-ONLY.
 *
 * WHY macho-ONLY rather than "unix": ✔MEASURED per name against the reference
 * header sets on the build host — musl (emsdk sysroot sys/types.h) and bionic
 * (Android NDK sysroot sys/types.h) declare NEITHER, and mingw-w64 declares
 * neither; only the macOS SDK (sys/types.h:108,128 — `typedef u_int32_t fixpt_t`
 * / `typedef int32_t segsz_t`) has them. They are the fixed-point-percentage and
 * segment-size types of BSD's process accounting, and the corpus reaches them
 * through <sys/sysctl.h>'s `struct extern_proc` (`fixpt_t p_pctcpu`,
 * `segsz_t e_xsize`), read as a REAL SDK header via the angle-include source
 * fallback while its own <sys/types.h> is intercepted by our descriptor.
 *
 * WHAT IS PINNED: the WIDTH of each (4 bytes) and — the axis a width pin cannot
 * see — the SIGNEDNESS SPLIT between them: fixpt_t is UNSIGNED (Darwin
 * u_int32_t) so (fixpt_t)-1 is positive, while segsz_t is SIGNED (int32_t) so
 * (segsz_t)-1 stays negative. SWAPPING THE TWO WOULD KEEP EVERY WIDTH ASSERT
 * GREEN, which is exactly why the split is asserted.
 *   ★ The signedness is asserted as a COMPARISON, not as
 *     `(fixpt_t)-1 == 4294967295u`: that equality is DEFEATED by C's usual
 *     arithmetic conversions (a signed -1 converts to the same unsigned value),
 *     so it would have been a vacuous pin.
 *   ★ Each spelling check is ONE _Generic carrying the right answer, the
 *     plausible wrong one, and a default — so "swapped" and "neither" are
 *     distinct failures. A pair of separate one-arm checks would be VACUOUS: the
 *     positive one always fires first and the negative one could never fail alone.
 *
 * exit 42 iff all hold, and every check below was ✔MEASURED to be REACHABLE by
 * some descriptor mutation — none rides along without an arm.
 * RED-ON-DISABLE (all ✔MEASURED against the final build): delete either name's
 * macho variant -> not injected -> S0006, no binary; SWAP the two types ->
 * exit 4; retype fixpt_t as u16 -> exit 2; retag `fixpt_t` as `u32 "wchar_t"`
 * -> exit 7; retag `segsz_t` as `i32 "wchar_t"` -> exit 9.
 * The ABSENCE side (these two must NOT appear on elf) is pinned by
 * shipped_sys_types_bsd_absent_elf. */
#include <sys/types.h>

int main(void) {
    fixpt_t f = 0;
    segsz_t s = 0;
    int     spelling;

    /* WIDTH */
    if (sizeof(f) != 4) return 2;
    if (sizeof(s) != 4) return 3;

    /* SIGNEDNESS — the split a width pin is blind to */
    f = (fixpt_t)-1;  if (!(f > 0)) return 4;   /* UNSIGNED: wraps positive */
    s = (segsz_t)-1;  if (!(s < 0)) return 5;   /* SIGNED: stays negative   */

    /* C-SPELLING IDENTITY, one _Generic each, naming the wrong answer */
    spelling = _Generic(f, unsigned int: 1, int: 2, default: 3);
    if (spelling == 2) return 6;   /* fixpt_t modelled SIGNED   */
    if (spelling != 1) return 7;   /* neither: wrong width/tag  */

    spelling = _Generic(s, int: 1, unsigned int: 2, default: 3);
    if (spelling == 2) return 8;   /* segsz_t modelled UNSIGNED */
    if (spelling != 1) return 9;

    return 42;
}
