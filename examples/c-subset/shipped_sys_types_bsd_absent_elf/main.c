// TF-C90 (D-CSUBSET-SYS-TYPES-BSD-SPELLING-GROUP-ABSENT) — the ABSENCE side of the
// BSD-ONLY pair: `fixpt_t` and `segsz_t` exist on macho and MUST NOT exist on elf.
//
// ✔MEASURED per name rather than assumed: musl (emsdk sysroot `sys/types.h`) and
// bionic (Android NDK sysroot `sys/types.h`) declare NEITHER — they are BSD
// process-accounting types (`u_int32_t fixpt_t` / `int32_t segsz_t`, macOS SDK
// sys/types.h:108,128). Shipping them on elf would be a descriptor claiming a
// name the platform's own header does not have, which is exactly the fidelity
// error this per-entry availability mechanism exists to avoid: each carries a
// SINGLE `macho` variant, and 0 matching variants means the typedef is not
// injected (the `off64_t` mechanism).
//
// ★ TWO POSITIVE CONTROLS, and they are what make this a measurement. `mode_t`
// (a flat entry) and `u_int` (an elf+macho `variants` entry) must BOTH still
// resolve on elf. `mode_t` proves the header resolved and the descriptor decoded;
// `u_int` proves the elf ARM of the variants mechanism itself selects — so the
// two absences below cannot be explained by "variants are broken on elf" or by
// the header not being read. Without these controls the manifest would pass
// vacuously the moment anything upstream stopped working.
//
// RED-ON-DISABLE: add an `elf` variant to `fixpt_t` or `segsz_t` -> it resolves
// -> its expected S_UnknownType does not appear -> this manifest fails. Delete
// `u_int`'s elf variant -> a THIRD unexpected S_UnknownType appears -> fails.
//
// ★ THERE IS DELIBERATELY NO pe TWIN OF THIS MANIFEST, and the reason is two
// measured harness defects rather than anything about these names:
// D-DIAG-PE-SPAN-LINE-MAPPING-SYNTHETIC-LINES (on pe64 a spanned diagnostic's
// reported LINE is shifted, and the in-process runner and the CLI disagree on the
// COLUMN) and D-TEST-POSITIONED-FALSE-REQUIRES-SPANLESS-RENDERING (`positioned:
// false` is unusable here because the integrated CLI arm greps for the SYMBOLIC
// rendering, which only SPAN-LESS diagnostics get). The pe absence is pinned by
// ShippedLibDescriptor.RealSysTypesBsdSpellingGroupPerFormat instead, and that
// arm reds ALONE when a pe variant is added.
#include <sys/types.h>

mode_t  aModeT;   /* POSITIVE CONTROL — flat entry, must NOT diagnose         */
u_int   aUInt;    /* POSITIVE CONTROL — elf variant selects, must NOT diagnose */

fixpt_t aFixpt;   /* BSD-only: absent from glibc/musl/bionic                  */
segsz_t aSegsz;

int main(void) {
    return 42;
}
