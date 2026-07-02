/* c89 (D-CSUBSET-SIZEOF-VALUE-OPERAND-TYPE): `sizeof <value-expr>` with an
 * OPERATOR in the operand (deref / index / member-through-deref / address-of
 * / binary) sizes the expression's RESULT type (C 6.5.3.4) — not the first
 * stamped leaf below it. Pre-fix: semantic Pass 2 stamped refs/members/
 * calls/casts/literals but NOT operator nodes, and lowerSizeof's
 * resolveStampedTypeBelow DFS sailed past the unstamped `*p` into the leaf
 * `p`: sizeof(*p) = 8 (the POINTER), sizeof(tab[0]) = sizeof(the WHOLE
 * array), ArraySize(X) = sizeof(X)/sizeof(X[0]) = 1. The sqlite detonation:
 * pthreadMutexAlloc's `sqlite3MallocZero( sizeof(*p) )` allocated 8 bytes
 * for the 40-byte recursive pthread mutex -> glibc's own mutex-init writes
 * clobbered the malloc top chunk -> sysmalloc SIGABRT on EVERY invocation
 * (the c88 smoke wall). FIX: the Pass-2 sizeofValueRule arm stamps the
 * operand with its full expression type via subtreeType (the same deriver
 * the Pass-1.5 array-dimension closure uses). Every row exits with a
 * DISTINCT code so a wrong size names its own shape.
 * RED-ON-DISABLE (audit-witnessed): rows 1/2/4/5/6/11 mis-size (r01=8
 * r02=8 r04=336 r05=1 r06=1 r11=100; ArraySize collapses to 1) — rows
 * 8/13 happened to coincide pre-fix (q->c was Pass-2-stamped; *q is 8=8).
 * => 42. NOTE: members are `double` (8 bytes on EVERY target) — `long`
 * would be 4 on pe64/LLP64 and break the grid's expected sizes. */
struct Big { double a, b, c, d, e, f; };        /* 48 bytes, all targets */
struct WithInt { int i; char c; };              /* 8 bytes */
static struct Big table[7];
static int itab[13];

int main(void) {
    struct Big *p = table;
    struct WithInt s;
    struct WithInt *q = &s;
    char buf[100];
    if (sizeof(*p) != 48) return 1;                        /* deref */
    if (sizeof(p[0]) != 48) return 2;                      /* index via ptr */
    if (sizeof(table) != 336) return 3;                    /* whole array */
    if (sizeof(table[0]) != 48) return 4;                  /* array element */
    if (sizeof(table) / sizeof(table[0]) != 7) return 5;   /* ArraySize */
    if (sizeof(itab) / sizeof(itab[0]) != 13) return 6;    /* ArraySize int */
    if (sizeof(s.i) != 4) return 7;                        /* member */
    if (sizeof(q->c) != 1) return 8;                       /* member via -> */
    if (sizeof(&s) != 8) return 9;                         /* address-of */
    if (sizeof(buf) != 100) return 10;                     /* char array */
    if (sizeof(buf[0]) != 1) return 11;                    /* char element */
    if (sizeof(s.i + 1) != 4) return 12;                   /* binary op */
    if (sizeof(*q) != 8) return 13;                        /* deref small */
    if (sizeof p != 8) return 14;                          /* bare ident */
    if (sizeof(s) != 8) return 15;                         /* bare struct */
    return 42;
}
