/* D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED — the
 * RUNTIME witness for the LEGITIMATE half of the rule.
 *
 * <memory.h> deliberately MIRRORS <string.h>'s mem* surface (it is the pre-ANSI
 * alias), so a TU that includes BOTH resolves TWO descriptors that declare
 * `memcpy`/`memmove`/`memset`/`memcmp`/`memchr` for the SAME object format. That
 * is exactly the shape cross-descriptor realization agreement compares, and the
 * shape sqlite's shell.c really has (it includes <string.h> at :166 and
 * <memory.h> at :7121). The sibling example `shipped_memory_h` includes ONLY the
 * alias, so it resolves ONE descriptor and never puts the two side by side.
 *
 * WHAT THIS PINS THAT A UNIT TEST CANNOT: that the agreement check does NOT
 * refuse the shipped configuration, and that the surviving import still WORKS —
 * on every leg, at RUNTIME, through both example runners and the release
 * pipeline. A checker that degenerated into "duplicates are forbidden" would fail
 * this example at BUILD time on all four targets.
 *
 * The include ORDER is <string.h> first, the opposite of nothing in the corpus
 * today: injection is first-wins by name, so string.json supplies the five mem*
 * externs here and memory.json's identical rows are deduped away. Both surfaces
 * are then USED — `strlen`/`strcmp`/`strchr` exist only in string.json, the mem*
 * five exist in both — so a wrong winner or a dropped surface changes the exit
 * code rather than merely the import table. */
#include <string.h>
#include <memory.h>

int main(void) {
    char src[8] = "dss-p42";   /* 7 chars + NUL */
    char dst[8];

    /* the DUPLICATED five — declared by BOTH descriptors, one extern minted */
    memcpy(dst, src, 8);
    if (memcmp(dst, src, 8) != 0) return 1;
    if (memchr(dst, '4', 8) == 0) return 2;
    memset(dst, 0, 8);
    if (dst[0] != 0 || dst[7] != 0) return 3;
    memmove(dst, src, 8);

    /* string.json's OWN surface — proves the winner did not take the alias's
     * narrower descriptor with it */
    if (strcmp(dst, src) != 0) return 4;
    if (strchr(src, 'p') == 0) return 5;

    return (int)strlen(src) + 35;   /* 7 + 35 = 42 */
}
