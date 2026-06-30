/* c68 (D-CSUBSET-AGGREGATE-GLOBAL-NONSYMBOL-PTR-MEMBER): a file-scope aggregate
 * global (an array of structs, like sqlite's built-in FuncDef tables) whose
 * members include the two link-time-constant shapes c67's classifier could not
 * fold, so the whole aggregate fell to runtime-init -> the aggregate-rvalue
 * fail-loud guard (D-CSUBSET-BITFIELD-RVALUE-RUNTIME). The two shapes:
 *
 *   (A) pUserData = INT_TO_PTR(X) = (void*)&((char*)0)[X]
 *       sqlite's SQLITE_INT_TO_PTR — stash a small integer X in a void* (read
 *       back via SQLITE_PTR_TO_INT). The address of element X of a NULL pointer
 *       base is the integer X*sizeof(elem) reinterpreted as a pointer: a
 *       pointer-valued INTEGER constant, NO symbol, NO relocation. Used for the
 *       pUserData member of aBuiltinFunc[]/aJsonFunc[] in sqlite3.c.
 *
 *   (B) zName = <arrayGlobalName>
 *       a bare reference to a file-scope `char[]` global, which DECAYS to its
 *       address &arr[0] -- a genuine link-time SYMBOL address (addend 0, an
 *       abs64 relocation). Used for aWindowFuncs[].zName (`name##Name`) in
 *       sqlite3.c.
 *
 * Verification is by POINTER IDENTITY (64-bit Ptr compares): pUserData must
 * hold the stashed integer (Class A); zName must point AT its array global
 * (Class B, the precise check that the decay-ref relocation resolved correctly).
 * No char-width comparison (that would trip the unrelated arm64 D-CSUBSET-32BIT-
 * ALU-FORMS narrow-ICmp limit). RED-ON-DISABLE: revert either fix -> the member
 * fails to classify -> the aggregate falls to runtime-init -> H_Unsupported-
 * LoweringForKind. gcc/clang compile this and exit 42.
 */

#define INT_TO_PTR(X) ((void*)&((char*)0)[X])

struct Entry {
    void       *pUserData;   /* (A) INT_TO_PTR */
    const char *zName;       /* (B) array-decay ref to a file-scope char[] */
};

static const char nameA[] = "alpha";
static const char nameB[] = "beta";
static const char nameC[] = "gamma";

static struct Entry table[] = {
    { INT_TO_PTR(7),  nameA },     /* A: int 7  in a void*;  B: &nameA[0] (reloc) */
    { INT_TO_PTR(11), nameB },     /* A: int 11 in a void*;  B: &nameB[0] (reloc) */
    { INT_TO_PTR(0),  nameC },     /* A: int 0  (== null);   B: &nameC[0] (reloc) */
};

int main(void) {
    /* Class A: the stashed integers round-trip (sqlite reads them via PTR_TO_INT). */
    if (table[0].pUserData != (void *)7)  return 1;
    if (table[1].pUserData != (void *)11) return 2;
    if (table[2].pUserData != (void *)0)  return 3;
    /* Class B: each zName's decay-ref must point AT its array global's bytes
     * (distinct first letters prove each reloc resolved to ITS own array — this
     * is how sqlite uses zName: read as a string, never pointer-compared). NOTE:
     * a pointer-IDENTITY check `zName == nameA` is deliberately NOT used here —
     * it trips a PRE-EXISTING, c68-independent PE array-decay miscompile (two
     * array-to-pointer decays of the same `static const char[]` compare unequal
     * on the pe target; reproduced with zero c68 code involved). */
    if ((int)table[0].zName[0] != 'a') return 4;   /* "alpha" */
    if ((int)table[1].zName[0] != 'b') return 5;   /* "beta"  */
    if ((int)table[2].zName[0] != 'g') return 6;   /* "gamma" */
    if ((int)table[0].zName[4] != 'a') return 7;   /* alph[a] (addend 0, exact) */
    if ((int)table[1].zName[3] != 'a') return 8;   /* bet[a]  */
    if ((int)table[2].zName[4] != 'a') return 9;   /* gamm[a] */
    return 42;
}
