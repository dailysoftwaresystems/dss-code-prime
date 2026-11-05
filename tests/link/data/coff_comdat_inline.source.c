/* P69 fold 3d -- the source of coff_comdat_inline_{a,b}_clangcl_O2.obj (D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION).
 *
 * A C99 `inline` function with a dense switch, as a header would give it to two units, each taking its address.
 * Both cl 19.44 and clang-cl 19.1.5 write it in every such unit as an EXTERNAL definition in a `.text` COMDAT of
 * selection ANY (one copy of several; the link keeps one), and clang-cl writes its jump table in an `.rdata` COMDAT
 * ASSOCIATIVE to that section (MEASURED 2026-10-21). So the two objects each carry a copy of `pick` AND of its table,
 * and a link must keep the table of the copy it keeps and drop the other copy's with it. `UNIT` names the unit and
 * its accessor (`addr_a` / `addr_b`).
 *
 * REBUILD (Windows, clang-cl 19.1.5 of the VS Build Tools, from tests/link/data):
 *   clang-cl /c /O2 /GS- /DUNIT=a coff_comdat_inline.source.c /Focoff_comdat_inline_a_clangcl_O2.obj
 *   clang-cl /c /O2 /GS- /DUNIT=b coff_comdat_inline.source.c /Focoff_comdat_inline_b_clangcl_O2.obj
 */
#ifndef UNIT
#error "UNIT names the unit: a or b"
#endif
#define ADDR_(u) addr_##u
#define ADDR(u)  ADDR_(u)

inline int pick(int k, int x) {
    switch (k) {
        case 0: return x + 3;
        case 1: return x * 5;
        case 2: return x - 7;
        case 3: return x ^ 11;
        case 4: return x << 2;
        case 5: return x / 3;
        case 6: return x % 13;
        case 7: return ~x;
        case 8: return x | 64;
        case 9: return x & 6;
        default: return -1;
    }
}

int (*ADDR(UNIT)(void))(int, int) { return pick; }
