/* c90 (D-CSUBSET-ASSIGN-VALUE-RHS-COERCE): a plain assignment used as a
 * VALUE (a comma operand, a chained assign, a condition, an operand of any
 * operator) coerces the RHS to the LVALUE's type before the store
 * (C 6.5.16p3: the value has the lvalue's type after conversion). Pre-fix
 * BOTH HIR value-position arms (finishAssign + lowerBinary's Assign arm)
 * stored at the RHS's width — the compound (OP=) arms beside them coerce
 * (c74), statement assigns + initializers coerce; exactly plain-`=`-as-value
 * was missed. Manifestations: (a) sub-int -> wider = the store writes only
 * the narrow bytes, the wider slot's UPPER bits stay STALE (the sqlite
 * detonation: estimateTableWidth's `for(i=pTab->nCol, ...)` i16->int comma
 * assign left i = 0x008c0005 instead of 5 -> a 3822-element walk past an
 * 80-byte array -> the SQL SIGSEGV); (b) wider -> sub-int = a full-width
 * store CLOBBERING the neighboring array elements (real memory corruption);
 * (c) chains compound the wrong width. dirty() pre-fills the stack with
 * nonzero junk so stale-upper-half reads cannot pass by zero-luck.
 * Each check exits with a DISTINCT code naming its shape.
 * RED-ON-DISABLE (remove the two coerce calls): checks 1/2/3/5/6/7 fail.
 * => 42. LLP64-safe: 8-byte cases use long long; the double source is a
 * volatile global (runtime, release-arm-safe). */

typedef short i16;
typedef unsigned short u16;
typedef unsigned char u8;

struct Col { char *z; u8 pad1; u8 pad2; u8 szEst; u8 pad3; int pad4; };
struct Tab { char *zName; struct Col *aCol; long long pad[5]; i16 iPKey; i16 nCol; };

static volatile double vdbl = 3.9;

static void dirty(void) {
    volatile int junk[128];
    int k;
    for (k = 0; k < 128; k++) junk[k] = 0x008c008c;
}

/* the exact sqlite estimateTableWidth shape: i16 member -> int, comma init */
static unsigned sqlite_shape(struct Tab *pTab) {
    unsigned wTable = 0;
    const struct Col *pTabCol;
    int i;
    for (i = pTab->nCol, pTabCol = pTab->aCol; i > 0; i--, pTabCol++) {
        wTable += pTabCol->szEst;
        if (wTable > 100000u) return 99999u;   /* runaway guard */
    }
    if (pTab->iPKey < 0) wTable++;
    return wTable;
}

/* int -> u8 element in comma-value position: neighbors must SURVIVE */
static int narrow_neighbors(int v) {
    u8 a[8];
    const char *q;
    static char nm[] = "x";
    int k;
    for (k = 0; k < 8; k++) a[k] = 0x5A;
    q = (a[2] = v, nm);
    (void)q;
    return (a[1] == 0x5A) && (a[2] == (u8)v) && (a[3] == 0x5A);
}

/* chained assign narrows through the i16 middle (C: a = (i16)b) */
static int chain_narrow(int v) {
    int a;
    i16 b;
    a = (b = v);
    return a;
}

/* triple chain through a junk-prefilled long long (partial-store witness) */
static long long chain_longlong(int v) {
    long long L;
    i16 b;
    int m;
    L = 0x7EFEFEFE12345678LL;
    m = (L = (b = v));
    return L + m;
}

/* u16 lvalue, int rhs, value position: the neighbors must survive */
static int u16_neighbors(int v) {
    u16 w[4];
    int k, x;
    for (k = 0; k < 4; k++) w[k] = 0xAAAA;
    x = ((w[1] = v), 1);
    (void)x;
    return (w[0] == 0xAAAA) && (w[1] == (u16)v) && (w[2] == 0xAAAA);
}

/* double -> int value assign (the volatile source keeps it runtime) */
static int dbl_to_int(void) {
    int i, x;
    x = ((i = vdbl), 1);
    (void)x;
    return i;
}

int main(void) {
    static struct Col cols[5];
    static struct Tab tab;
    int k;
    for (k = 0; k < 5; k++) { cols[k].z = 0; cols[k].szEst = (u8)(k + 3); }
    tab.zName = 0; tab.aCol = cols; tab.iPKey = -1; tab.nCol = 5;

    dirty();
    if (sqlite_shape(&tab) != 26u) return 1;      /* 3+4+5+6+7 + 1 = 26 */
    dirty();
    if (!narrow_neighbors(-2)) return 2;          /* (u8)-2 = 254, 0x5A intact */
    dirty();
    if (chain_narrow(70000) != 4464) return 3;    /* 70000 & 0xFFFF = 4464 */
    dirty();
    if (chain_longlong(-5) != -10) return 4;      /* L=-5 fully, m=-5 */
    dirty();
    if (!u16_neighbors(70000)) return 5;          /* w[1]=4464, 0xAAAA intact */
    dirty();
    if (dbl_to_int() != 3) return 6;              /* (int)3.9 = 3 */
    dirty();
    { /* the raw stale-upper witness: i16 -> int comma assign */
        i16 v = 5;
        const char *q;
        int i;
        static char nm[] = "x";
        q = (i = v, nm);
        (void)q;
        if (i != 5) return 7;
    }
    return 42;
}
