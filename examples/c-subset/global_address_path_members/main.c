/* c80 repro C — the SAME symbol-base path shapes in their remaining
 * positions, closing D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-BASE-INDEX fully:
 *   (1) `&arr[K]` as an AGGREGATE MEMBER (the anchor's original context),
 *   (2) `&record.field` (AddressOf(MemberAccess)) top-level + as a member,
 *   (3) a NESTED path `&grid[i][j]` (Index-of-Index composition).
 *
 * All are address constants (C 6.6p9); gcc emits abs64 relocs with the
 * element/field-offset addend. gcc -O0/-O2 exits 42.
 */
static const unsigned char letters[8] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
static int grid[3][4] = {
    { 1,  2,  3,  4 },
    { 5,  6,  7,  8 },
    { 9, 10, 11, 12 },
};

struct Rec { int a; int b; unsigned char tail[4]; };
static struct Rec rec = { 7, 9, { 21, 22, 23, 24 } };

/* (2) top-level field addresses */
static int *pB = &rec.b;
static unsigned char *pTail2 = &rec.tail[2];   /* member-then-index path */

/* (3) nested index */
static int *pGrid = &grid[2][1];               /* -> 10 */

/* (1) the shapes as AGGREGATE members */
struct Slot { const unsigned char *pc; int *pi; };
static struct Slot slots[2] = {
    { &letters[3], &grid[1][2] },              /* 'd', 7 */
    { &letters[8 - 2], 0 },                    /* 'g'     */
};

int main(void) {
    if (*pB != 9) return 1;
    *pB = 33;                                  /* write-through must hit rec.b */
    if (rec.b != 33) return 2;
    if (*pTail2 != 23) return 3;
    if (*pGrid != 10) return 4;
    if (*slots[0].pc != 'd') return 5;
    if (*slots[0].pi != 7) return 6;
    if (*slots[1].pc != 'g') return 7;
    if (slots[1].pi != 0) return 8;
    return 42;
}
