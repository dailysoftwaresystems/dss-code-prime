/* D-CSUBSET-BITFIELD-ASSIGN-VALUE-POSITION end-to-end RUNTIME witness across all
 * targets. A bit-field MUTATION is a read-modify-write of the allocation unit. It
 * was correct ONLY for statement plain-`=`; every OTHER form — a VALUE-position
 * `(bf.a = v)` / `(bf.a += 1)` / `(bf.a++)` / `(++bf.a)` and a STATEMENT compound /
 * inc-dec `bf.a += 1;` / `bf.a++;` / `--bf.a;` — took a full-unit store instead,
 * clobbering the packed neighbour and skipping truncation, and (value forms) yielded
 * the UN-truncated value. This exercises every form and asserts:
 *   - VALUE forms yield the STORED (truncated) value: `(g.a = 300)` == 300 & 0xF = 12;
 *     `(g.a += 7)` == (12+7)&0xF = 3; `(g.a++)` == the OLD value; `(++g.a)` == the NEW;
 *   - the packed NEIGHBOUR `g.b` SURVIVES every a-mutation (RMW, not over-wide store);
 *   - a NON-bit-field member (`g.plain`) stays correct through the SAME reconstruction
 *     path (a plain scalar store — the #1 blast-radius guard);
 *   - the base of `ga[i++].a += 6` is evaluated EXACTLY ONCE (i ends 1, ga[0] mutated,
 *     ga[1] untouched).
 * Each failure returns a distinct non-42 code; a correct compile returns
 * acc(23) + g.a(14) + g.b(5) - ga[1].a(0) = 42. The exit is LAYOUT-RULE-AGNOSTIC
 * (write+read share the packing). The optimizedPipelines arm proves the flow
 * survives Mem2Reg/ConstFold; the baseline arm is the real-codegen witness. */
struct Grid {
    unsigned a : 4;    /* the mutated bit-field (mask 0xF) */
    unsigned b : 4;    /* packed NEIGHBOUR — must survive every a-mutation */
    int      plain;    /* an ordinary (non-bit-field) member — regression guard */
};

int main(void) {
    struct Grid g;
    g.b = 5;               /* neighbour, written first; must stay 5 */
    g.plain = 40;

    int acc = 0;

    /* (1) value-position plain `=` yields the TRUNCATED stored value. */
    acc += (int)(g.a = 300);      /* 300 & 0xF = 12; g.a = 12 -> acc 12 */

    /* (2) value-position compound `+=`. */
    acc += (int)(g.a += 7);       /* (12+7)&0xF = 3; g.a = 3  -> acc 15 */

    /* (3) value-position POSTFIX ++ yields the OLD value. */
    acc += (int)(g.a++);          /* OLD 3; g.a = 4           -> acc 18 */

    /* (4) value-position PREFIX ++ yields the NEW value. */
    acc += (int)(++g.a);          /* NEW 5; g.a = 5           -> acc 23 */

    /* (5) STATEMENT compound / inc / dec (produced value discarded). */
    g.a += 9;                     /* (5+9)&0xF = 14 */
    g.a++;                        /* 15 */
    --g.a;                        /* 14 */

    /* (6) NON-bit-field member through the SAME member reconstruction path. */
    g.plain += 2;                 /* 42 */
    g.plain--;                    /* 41 */

    /* (7) side-effect-once: the base `ga[i++]` is evaluated EXACTLY once. */
    struct Grid ga[2];
    ga[0].a = 0; ga[0].b = 0;
    ga[1].a = 0; ga[1].b = 0;
    int i = 0;
    ga[i++].a += 6;               /* i -> 1, ga[0].a = 6, ga[1] untouched */

    if (acc != 23)                     return 1;  /* value-yield semantics */
    if ((int)g.a != 14)                return 2;  /* statement forms */
    if ((int)g.b != 5)                 return 3;  /* neighbour survived */
    if (g.plain != 41)                 return 4;  /* non-bit-field member intact */
    if (i != 1)                        return 5;  /* single evaluation */
    if ((int)ga[0].a != 6)             return 6;  /* right element mutated */
    if ((int)ga[1].a != 0)             return 7;  /* other element untouched */

    return acc + (int)g.a + (int)g.b - (int)ga[1].a;  /* 23 + 14 + 5 - 0 = 42 */
}
