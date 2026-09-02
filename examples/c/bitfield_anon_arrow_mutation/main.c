/* D-CSUBSET-BITFIELD-ANON-ARROW-MUTATION-RESIDUAL end-to-end RUNTIME witness.
 *
 * A bit-field MUTATION is a read-modify-write of the packed allocation unit.
 * D-CSUBSET-BITFIELD-ASSIGN-VALUE-POSITION made that true for a NAMED `.`/`->`
 * member, by binding the CONTAINING AGGREGATE's address and rebuilding the
 * `MemberAccess` the MIR chokepoint keys on. Two bases could not be named by that
 * single-index lvalue and were REFUSED (loudly — never miscompiled):
 *
 *   (A) a field behind one or more ANONYMOUS struct/union members (C11/C23
 *       6.7.2.1 p13) — `o.a` is really `o.<anon>.a`, an intermediate hop;
 *   (B) an ARRAY-arrow decay base (C 6.3.2.1 p3) — `sarr->a`, where the array
 *       decays to `&sarr[0]` before the arrow's deref.
 *
 * ✔MEASURED 2026-08-31: gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0 all
 * COMPILE AND RUN both shapes correctly; MSVC 19.51 accepts both. The lvalue now
 * carries an ordered member-hop CHAIN, so both reconstruct.
 *
 * This asserts, for BOTH bases and for a two-level NESTED anonymous chain:
 *   - VALUE forms yield the STORED (truncated) value: `(o.a = 300)` == 300 & 0xF;
 *     `(o.a += 7)`, post-`++` yields the OLD value, pre-`++` the NEW;
 *   - the packed NEIGHBOUR survives every mutation (RMW, not an over-wide store);
 *   - a NON-bit-field member through the SAME two bases stays a plain scalar
 *     store (the blast-radius guard — both bases changed path with this fix);
 *   - an adjacent ARRAY ELEMENT is untouched by an array-arrow mutation;
 *   - the ARRAY base of `g2[bump()]->a += 1` is evaluated EXACTLY ONCE.
 * Each failure returns a distinct non-42 code. The exit is LAYOUT-RULE-AGNOSTIC
 * (write and read share whatever packing the target's rules give). The
 * optimizedPipelines arms prove the flow survives Mem2Reg/ConstFold and the real
 * shipped `release` pipeline; the baseline arm is the real-codegen witness. */

struct Anon {
    struct { unsigned a : 4; unsigned b : 4; };  /* (A) anonymous struct member */
    int plain;                                   /* non-bit-field regression guard */
};

struct Nested {
    struct { struct { unsigned a : 4; unsigned b : 4; }; };  /* TWO anon levels */
};

struct Cell {
    unsigned a : 4;
    unsigned b : 4;   /* packed NEIGHBOUR — must survive every a-mutation */
    int      plain;
};

static struct Cell g[2];      /* (B) array-arrow decay base */
static struct Cell g2[2][2];  /* `g2[i]` is an ARRAY → arrow-decays */
static int evals;

static int bump(void) { evals += 1; return 0; }

int main(void) {
    /* ── (A) the ANONYMOUS-member hop chain ─────────────────────────────── */
    struct Anon o;
    o.b = 5;                       /* neighbour; must stay 5 throughout */
    o.plain = 40;
    int acc = 0;

    acc += (int)(o.a = 300);       /* value `=`: 300 & 0xF = 12 -> acc 12 */
    acc += (int)(o.a += 7);        /* value `+=`: (12+7)&0xF = 3 -> acc 15 */
    acc += (int)(o.a++);           /* value post-++: OLD 3, o.a = 4 -> acc 18 */
    acc += (int)(++o.a);           /* value pre-++: NEW 5          -> acc 23 */
    o.a += 9;                      /* statement forms: (5+9)&0xF = 14 */
    o.a++;                         /* 15 */
    --o.a;                         /* 14 */
    o.plain += 2;                  /* a NON-bit-field member behind the SAME */
    o.plain--;                     /* anonymous base: a plain scalar store -> 41 */

    if (acc != 23)          return 1;   /* anon value-yield semantics */
    if ((int)o.a != 14)     return 2;   /* anon statement forms */
    if ((int)o.b != 5)      return 3;   /* anon packed neighbour survived */
    if (o.plain != 41)      return 4;   /* anon non-bit-field member intact */

    /* TWO anonymous levels — the hop chain is a loop, not a special case. */
    struct Nested n;
    n.b = 9;
    n.a = 15;
    n.a += 1;                      /* wraps within 4 bits: 0 */
    if ((int)n.a != 0)      return 5;   /* nested-anon truncation */
    if ((int)n.b != 9)      return 6;   /* nested-anon neighbour survived */

    /* ── (B) the ARRAY-ARROW decay base ─────────────────────────────────── */
    g[0].b = 5;  g[0].plain = 40;
    g[1].a = 7;  g[1].b = 3;       /* the adjacent ELEMENT must not move */
    int bacc = 0;

    bacc += (int)(g->a = 300);     /* 12 -> bacc 12 */
    bacc += (int)(g->a += 7);      /* 3  -> bacc 15 */
    bacc += (int)(g->a++);         /* OLD 3, g->a = 4 -> bacc 18 */
    bacc += (int)(++g->a);         /* NEW 5           -> bacc 23 */
    g->a += 9;                     /* 14 */
    g->a++;                        /* 15 */
    --g->a;                        /* 14 */
    g->plain += 2;                 /* a NON-bit-field member through the SAME */
    g->plain--;                    /* array-arrow base -> 41 */

    if (bacc != 23)         return 7;   /* arrow value-yield semantics */
    if ((int)g->a != 14)    return 8;   /* arrow statement forms */
    if ((int)g->b != 5)     return 9;   /* arrow packed neighbour survived */
    if (g->plain != 41)     return 10;  /* arrow non-bit-field member intact */
    if ((int)g[1].a != 7)   return 11;  /* adjacent ARRAY ELEMENT untouched */
    if ((int)g[1].b != 3)   return 12;

    /* side-effect-once: the ARRAY base `g2[bump()]` is evaluated EXACTLY once. */
    g2[0][0].a = 1;  g2[0][0].b = 6;
    g2[1][0].a = 2;
    evals = 0;
    g2[bump()]->a += 1;
    if (evals != 1)         return 13;  /* single evaluation of the array base */
    if ((int)g2[0][0].a != 2) return 14; /* the right cell mutated */
    if ((int)g2[0][0].b != 6) return 15; /* its neighbour survived */
    if ((int)g2[1][0].a != 2) return 16; /* the other row untouched */

    return acc + (int)o.a + (int)g->b;   /* 23 + 14 + 5 = 42 */
}
