/* TF-C90 (D-CSUBSET-SYS-TYPES-BSD-SPELLING-GROUP-ABSENT) — the shipped
 * <sys/types.h> BSD-COMPAT SPELLING GROUP on the UNIX legs (elf + macho): the
 * seven names every unix <sys/types.h> declares and the Windows one declares not
 * at all.
 *
 * WHY IT EXISTS. `sys/types.json` WINS over the platform's real <sys/types.h>
 * (include_path_resolve.hpp: a shipped descriptor is priority 1, a real source
 * header on -I is only the descriptor-MISS fallback), so a name the descriptor
 * omits can NEVER be declared no matter what the SDK header holds. Before this
 * cycle the descriptor declared none of these, and the macOS SDK headers DSS
 * reads through the angle-include source fallback — <sys/sysctl.h>'s
 * `struct extern_proc` / `int sysctl(int *, u_int, …)` — spell them freely.
 *
 * WHAT IS PINNED. Width + signedness + C spelling together determine a scalar
 * type completely, so all three are asserted and nothing weaker:
 *   (a) WIDTH — sizeof each name.
 *   (b) SIGNEDNESS — `(T)-1 > 0` for every unsigned member, `< 0` for the signed
 *       one. This is the axis a sizeof pin is blind to (u32 vs i32 are both 4).
 *       ★ It is asserted as a COMPARISON, not as `(T)-1 == 4294967295u`: that
 *       equality is DEFEATED by C's usual arithmetic conversions (a signed -1
 *       converts to the same unsigned value and the check passes anyway), so it
 *       would have been a vacuous pin.
 *   (c) C-SPELLING IDENTITY via _Generic. For u_long — whose tag is the one this
 *       group actually carries — ONE _Generic holds the RIGHT spelling AND the
 *       plausible WRONG one AND a default, so "mis-tagged" (15) and "untagged"
 *       (16) are DISTINCT failures. ★ Two separate one-arm checks would have been
 *       VACUOUS: the positive one always fires first, so the negative one could
 *       never fail on its own.
 *   (d) caddr_t addresses real char storage — a write through it is visible in
 *       the array it points at (codegen truth, not just frontend type truth).
 *
 * exit 42 is the witness that all four hold. Every failure has its OWN exit code
 * so a red arm names itself, and every check below was ✔MEASURED to be REACHABLE
 * by some descriptor mutation — none rides along without an arm.
 *
 * RED-ON-DISABLE (each ✔MEASURED against the final build): delete the elf+macho
 * `variants` of any of the seven -> that name is not injected -> S0006, no
 * binary. u_int -> i32 -> exit 11 (signedness). u_long retagged
 * `unsigned long long` -> exit 15. u_long's tag dropped -> exit 16. u_short
 * retagged `u16 "wchar_t"` -> exit 18. caddr_t -> ptr<void> / ptr<u8> /
 * ptr<i8> / ptr<byte> -> S0003 at the initializer, no binary. A PHANTOM tag
 * (`u32 "unsigned long"`, unproducible under LP64) is caught even earlier by the
 * existing F_ShippedTypeIdentityConflict consistency check.
 *
 * NO pe arm: this group is absent on pe BY DESIGN (mingw-w64 <sys/types.h>
 * declares none of it). ★ That side is pinned by the UNIT sibling
 * ShippedLibDescriptor.RealSysTypesBsdSpellingGroupPerFormat and NOT by a corpus
 * error manifest, because of TWO measured blockers that are not about this group:
 *   D-DIAG-PE-SPAN-LINE-MAPPING-SYNTHETIC-LINES — on the pe64 target a
 *     source-spanned diagnostic's reported LINE is SHIFTED (a 4-line file's line 3
 *     is reported as 5; as 8 once one #include is added; elf and macho report 3 in
 *     both), and the in-process runner and the CLI harness then disagree on the
 *     COLUMN too (31:11 vs 31:1 on the same diagnostic), so no honest line:col
 *     exists to assert.
 *   D-TEST-POSITIONED-FALSE-REQUIRES-SPANLESS-RENDERING — `positioned:false` is
 *     not an escape: the integrated CLI arm greps for the SYMBOLIC rendering
 *     `error[S_UnknownType]`, which the CLI emits only for SPAN-LESS diagnostics —
 *     a spanned one renders `error[S0006]`.
 * The corpus pe arm lands when either is fixed. Baking the shifted numbers into a
 * golden manifest would have encoded the first defect as EXPECTED behaviour.
 * The two BSD-ONLY members (fixpt_t / segsz_t, absent from glibc/musl/bionic)
 * live in shipped_sys_types_bsd_macho + shipped_sys_types_bsd_absent_elf. */
#include <sys/types.h>

int main(void) {
    u_char   uc = 0;
    u_short  us = 0;
    u_int    ui = 0;
    u_long   ul = 0;
    quad_t   q  = 0;
    u_quad_t uq = 0;
    char     buf[2];
    caddr_t  ca = buf;
    int      spelling;

    /* (a) WIDTH */
    if (sizeof(uc) != 1)              return 2;
    if (sizeof(us) != 2)              return 3;
    if (sizeof(ui) != 4)              return 4;
    if (sizeof(ul) != 8)              return 5;   /* LP64 unsigned long */
    if (sizeof(q)  != 8)              return 6;
    if (sizeof(uq) != 8)              return 7;
    if (sizeof(ca) != sizeof(char *)) return 8;

    /* (b) SIGNEDNESS — the axis sizeof cannot see */
    uc = (u_char)-1;    if (!(uc > 0)) return 9;
    us = (u_short)-1;   if (!(us > 0)) return 10;
    ui = (u_int)-1;     if (!(ui > 0)) return 11;
    ul = (u_long)-1;    if (!(ul > 0)) return 12;
    uq = (u_quad_t)-1;  if (!(uq > 0)) return 13;
    q  = -1;            if (!(q  < 0)) return 14;  /* quad_t is SIGNED */

    /* (c) C-SPELLING IDENTITY.
     * u_long: ONE _Generic naming both long spellings + a default, so a WRONG
     * tag (15) and NO tag (16) are separately visible. */
    spelling = _Generic(ul, unsigned long: 1, unsigned long long: 2, default: 3);
    if (spelling == 2) return 15;   /* mis-tagged `unsigned long long`      */
    if (spelling != 1) return 16;   /* untagged: matches NEITHER spelling   */

    /* the three small unsigned members: a single arm + default, which IS
     * two-sided here — ✔MEASURED that the default arm is REACHABLE (retag
     * u_short as `u16 "wchar_t"` and this fires exit 18), so these are the
     * checks that see a wrong TAG on a right-width, right-signedness type. */
    if (_Generic(uc, unsigned char:  1, default: 0) != 1) return 17;
    if (_Generic(us, unsigned short: 1, default: 0) != 1) return 18;
    if (_Generic(ui, unsigned int:   1, default: 0) != 1) return 19;

    /* ★ caddr_t deliberately gets NO _Generic check. It would have been a check
     * WITH NO ARM: ✔MEASURED that EVERY wrong pointer model (ptr<void>,
     * ptr<u8>, ptr<i8>, ptr<byte>) is rejected at the `caddr_t ca = buf;`
     * initializer with S0003 before any _Generic could run, so the assertion
     * could never fail on its own. Its identity is pinned by what CAN fire:
     * initializing from `char[2]`, sizeof == sizeof(char *), and the store
     * below — all three red on those same mutations. */

    /* (d) caddr_t addresses real char storage */
    ca[0] = 'A';
    ca[1] = 'B';
    if (buf[0] != 'A' || buf[1] != 'B') return 20;

    /* 64-bit arithmetic really is 64-bit through the quad spellings */
    q  = (quad_t)1   << 62;  if (!(q  > 0)) return 21;
    uq = (u_quad_t)1 << 63;  if (uq == 0)   return 22;

    return 42;
}
