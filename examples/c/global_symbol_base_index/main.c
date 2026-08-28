/* c80 Family A repro — TOP-LEVEL symbol-base array-index address constant
 * (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-BASE-INDEX, the scalar form).
 *
 * sqlite3.c:24077-24079:
 *   SQLITE_PRIVATE const unsigned char *sqlite3aLTb = &sqlite3UpperToLower[256-OP_Ne];
 *   SQLITE_PRIVATE const unsigned char *sqlite3aEQb = &sqlite3UpperToLower[256+6-OP_Ne];
 *   SQLITE_PRIVATE const unsigned char *sqlite3aGTb = &sqlite3UpperToLower[256+12-OP_Ne];
 *
 * `&arr[CONST-EXPR]` is an ADDRESS CONSTANT (C 6.6p9): gcc emits
 * `.quad arr+K*sizeof(elem)` (abs64 reloc with addend), never a runtime store.
 * Pre-fix: tryClassifyAsSymbolAddr's AddressOf arm accepted only AddressOf(Ref)
 * -> the init fell to runtimeInit -> K_NoMatchingObjectFormat fail-loud.
 *
 * Checks are BY VALUE (deliberately no `p == arr` pointer-identity check:
 * that trips the pre-existing, unrelated D-CSUBSET-ARRAY-DECAY-POINTER-IDENTITY
 * deferral). gcc -O0/-O2 exits 42.
 */
#define OP_Ne 5   /* scaled-down mirror of sqlite's opcode arithmetic */

static const unsigned char tbl[16 + 3] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 100, 101, 102
};
/* the sqlite trio shape: Sub / Add(Sub) index expressions */
static const unsigned char *pLT = &tbl[16 - OP_Ne];       /* &tbl[11] */
static const unsigned char *pEQ = &tbl[16 + 1 - OP_Ne];   /* &tbl[12] */
static const unsigned char *pGT = &tbl[16 + 2 - OP_Ne];   /* &tbl[13] */

/* stride > 1: the addend must scale by sizeof(int) */
static const int itbl[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
static const int *ip = &itbl[8 - 2];                      /* &itbl[6] -> 70 */

int main(void) {
    if (*pLT != 11) return 1;
    if (*pEQ != 12) return 2;
    if (*pGT != 13) return 3;
    if (pLT[5] != 100) return 4;   /* reads across into the appended tail */
    if (*ip != 70) return 5;
    if (ip[1] != 80) return 6;
    return 42;
}
