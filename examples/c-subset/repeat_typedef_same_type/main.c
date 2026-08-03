/* TF-C97 (D-CSUBSET-REPEAT-TYPEDEF-SAME-TYPE) — runtime witness for C11 6.7p3: "a typedef name may be redefined to denote the same type as it currently does, provided that type is not a variably modified type".
 *
 * Two arms, one exit code, and BOTH arms only compile if the repeat is admitted:
 *
 *   (1) THE REAL SDK SHAPE — one tag, two headers. `$SDK/usr/include/malloc/
 *       _malloc_type.h:79` typedefs the FORWARD-DECLARED `struct _malloc_zone_t`;
 *       `malloc/malloc.h:246` typedefs the SAME tag AT ITS COMPLETION
 *       (`typedef struct _malloc_zone_t { … } malloc_zone_t;`). After preprocessing
 *       they are two same-scope typedefs of one name — MEASURED in sqlite's mem1.c,
 *       and the last S0002 on the arm64-macho sqlite leg. Reproduced here with
 *       `zone_t`. The two SPELLINGS differ (a bare tag reference vs the tag's
 *       definition), so only comparing RESOLVED types can accept it.
 *
 *   (2) THE DIRECT REPEAT — `typedef int Count;` written twice, byte-identically.
 *
 * WHAT MAKES THE EXIT LOAD-BEARING rather than decorative: `zone_total` is
 * PROTOTYPED between the two typedefs (so its parameter resolves through alias #1,
 * against the INCOMPLETE tag) and DEFINED after them (resolving through alias #2,
 * against the COMPLETED tag). That proto/def pair merges only if the two aliases are
 * ONE type — if they were distinct, the post-Pass-1.5 signature sweep would fire
 * S_IncompatibleRedeclaration and there would be no binary to run. `tag_total`
 * reaches the same fields through the TAG spelling (`struct _zone_t *`), so all
 * three spellings — alias #1, alias #2, the tag — must name one type for 21+21.
 *
 * ANTI-FOLD: `g_seed` is a mutable global (runtime-opaque) and every field value is
 * derived from it across a real call boundary, so no pass can const-fold the two
 * calls to `return 42`. The `release` arm re-runs the SHIPPED release pipeline
 * (Mem2Reg + Inlining active) over the same source.
 *
 * RED-ON-DISABLE (MEASURED, not predicted): drop `|| typedefRepeat` from the Pass-1
 * merge gate in mergeOrCollideRedeclaration (semantic_analyzer.cpp) and BOTH `zone_t`
 * and `Count` fall into the bothDefinitions collision arm — 2x error[S0002]
 * "redeclared symbol", the program no longer COMPILES, and the runner reports a
 * compile failure instead of exit 42. The verbatim SDK extract behaves identically:
 * with the admission reverted, the real `malloc_zone_t` pair pulled out of
 * `clang -E` output is exactly `1 x error[S0002]: got malloc_zone_t`.
 *
 * NOT WIDENED, and each is pinned in test_semantic_analyzer_c_subset.cpp: a repeat
 * of a VARIABLY MODIFIED type stays loud (6.7p3's carve-out — S0002, matching
 * clang's "redefinition of typedef for variably-modified type"); two DIFFERENT types
 * under one name stay loud (S0022); and a typedef repeating a NON-typedef name stays
 * a cross-category collision (S0002).
 *
 * exit = 21 + 21 = 42.
 */

/* Runtime-opaque seed: defeats const-folding the two calls below. */
int g_seed = 20;

/* ── "header A": the tag, forward-declared, then aliased ── */
struct _zone_t;
typedef struct _zone_t zone_t;

/* Prototyped against alias #1 — the tag is still INCOMPLETE at this point. */
int zone_total(zone_t *z);

/* ── "header B": the SAME tag, completed, and aliased AGAIN under one name ──
 * This second typedef is the C11 6.7p3 repeat. */
typedef struct _zone_t {
    int version;
    int size;
} zone_t;

/* ── Arm 2: the direct byte-identical repeat ── */
typedef int Count;
typedef int Count;

/* Defined against alias #2. Merges with the prototype above ONLY if alias #1 and
 * alias #2 denote one type. */
int zone_total(zone_t *z) {
    return z->version + z->size;
}

/* The TAG spelling reaches the same fields — third spelling, same type. */
static int tag_total(struct _zone_t *z) {
    return z->version + z->size;
}

int main(void) {
    zone_t z;
    z.version = g_seed;      /* 20 */
    z.size    = g_seed - 19; /*  1 */

    Count viaAlias = zone_total(&z);  /* 21, across the proto/def merge */
    Count viaTag   = tag_total(&z);   /* 21, through the tag spelling   */

    return viaAlias + viaTag;         /* 42 */
}
