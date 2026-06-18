/* D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL + D-CSUBSET-STRUCT-BODY-VARDECL-POSITION
 * — the struct-head closing cycle's RUNTIME witness. It exercises the head-grammar
 * forms the cycle opened AND the `encodeAggregateValue` rodata-encoder shapes that
 * a c-subset source can actually REACH, each as a BYTE-EXACT pin: a wrong offset /
 * wrong value / bad stride flips one summand and the exit code, so a byte-encoding
 * regression cannot survive. (The encoder's omitted-slot/short-init zero-fill — which
 * HIR pre-normalizes away before it reaches the encoder — and its fail-loud arms are
 * UNREACHABLE from c-subset, so they are pinned synthetically at the asm tier:
 * tests/asm/test_asm_substrate.cpp `AsmAggregateGlobal.*`.)
 *
 * Grammar forms:
 *   - a BARE top-level struct definition (declares only the tag → TypeDecl, no object);
 *   - a BARE tag-REF top-level return head (`struct Tri make(void)`).
 * Aggregate-global encoder shapes reachable from c-subset (all const-init → `.rodata`):
 *   - PADDING:      `gpad.val` sits at offset 4 (after `char` + 3 pad bytes) — reading
 *                   19 back proves the int landed at the aligned offset, not at 1;
 *   - EXPLICIT-ZERO: `gpart`'s b,c are written `0` and read back 0 — the encoder must
 *                    place every field at its offset (HIR delivers a FULL field list,
 *                    so this is a written-zero field, not an omitted-slot fill);
 *   - UNION:        a brace-init writes the FIRST member (`gun.i`), slack stays zero;
 *   - NESTED:       `gnest` is a struct-in-struct aggregate init;
 *   - ARRAY:        `gv[3]` element stride.
 *
 * Exit = gpad.val(19) + (gpart.a+b+c = 10) + gun.i(6) + nested(4) + arr(3) = 42. */

struct Meta { int version; };                          /* bare top-level def → TypeDecl */

struct Padded { char tag; int val; } gpad = { 1, 19 };          /* PADDING: int @ offset 4 */
struct Triple { int a; int b; int c; } gpart = { 10, 0, 0 };    /* EXPLICIT-ZERO fields b,c */
union  Tag    { int i; int j; }        gun  = { 6 };            /* UNION: first-member brace-init */
struct Inner  { int x; int y; };
struct Outer  { struct Inner in; int z; } gnest = { { 1, 2 }, 1 };  /* NESTED aggregate init */
int gv[3] = { 1, 1, 1 };                                        /* ARRAY element stride */

struct Tri { int a; int b; int c; };
struct Tri make(void) {                                /* bare tag-REF top-level return head */
    struct Tri t;
    t.a = gpad.val;                          /* 19 — padded-struct int read at the aligned offset */
    t.b = gpart.a + gpart.b + gpart.c;       /* 10 — explicit-zero fields b,c must read 0 */
    t.c = gun.i;                             /*  6 — union first member */
    return t;
}

int main(void) {
    struct Tri t = make();
    int nested = gnest.in.x + gnest.in.y + gnest.z;    /* 4 */
    int arr    = gv[0] + gv[1] + gv[2];                /* 3 */
    return t.a + t.b + t.c + nested + arr;
}
