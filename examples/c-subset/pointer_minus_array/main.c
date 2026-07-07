/* c65 (D-CSUBSET-POINTER-DIFF-EDGE-CASES, C 6.5.6p9 + 6.3.2.1p3): `pointer -
 * arrayName` is a pointer DIFFERENCE (ptrdiff_t = the ELEMENT count) — the array
 * operand decays to Ptr<elem>, then it is `p - q`. Frontier sqlite vdbeSorter
 * `pSorter->iPrev = (u8)(pTask - pSorter->aTask);` (sqlite3.c:107252) — a pointer
 * minus a struct-member array. c59 deferred this (its array-decay fired only when
 * the OTHER operand was a scalar index); the un-decayed Array operand made
 * ptrIntArith true, so the MIR `p±n` branch tried to widen the Array index via
 * mapCast(Array,I64) → MirOpcode::Invalid → addInst std::abort()'d the whole
 * compiler (exit 134, no diagnostic). The fix decays the array operand of Sub to
 * Ptr<elem> at the HIR tier → the existing c40 ptrSub path (PtrToInt+Sub+SDiv by
 * sizeof(elem)). A 16-byte element makes the stride DIVISION load-bearing: a
 * missing decay/division would yield the BYTE difference, not the element count.
 * The decay fires ONLY when the element/pointee types MATCH; a MISMATCHED
 * pairing (`int* - char[]`, which gcc rejects) is left un-decayed → it fails
 * LOUD (S0008 / the H0009 guard, per context), never a silent miscompile.
 * RED-ON-DISABLE: revert the HIR Sub array-decay arm → the corpus fails to
 * compile — a clean H0009 ("index TypeKind 21 has no widening cast") via the c65
 * MIR fail-loud guard; reverting that guard too → the original addInst
 * std::abort() (exit 134, the c64-reprobe crash). */

struct Task { long lo; long hi; };           /* 16 bytes → stride division matters */
struct Pool { struct Task aTask[8]; };

/* the EXACT sqlite shape: a pointer minus a STRUCT-MEMBER array */
int idx_in_pool(struct Pool *p, struct Task *t){
    return (int)(t - p->aTask);
}

int keep(int x){ return x; }                 /* keep indices runtime-opaque */

int main(void){
    struct Pool pool;

    /* ptr - member-array (the sqlite construct): the ELEMENT index, NOT the byte
     * offset (48 bytes / 16 = 3). idx_in_pool's operands are params → the diff is
     * a genuine RUNTIME pointer subtraction, not a const-fold. */
    if (idx_in_pool(&pool, &pool.aTask[3]) != 3) return 1;
    if (idx_in_pool(&pool, &pool.aTask[0]) != 0) return 2;
    if (idx_in_pool(&pool, &pool.aTask[7]) != 7) return 3;

    /* ptr - bare arrayName */
    struct Task arr[5];
    struct Task *e = &arr[keep(4)];
    if ((int)(e - arr) != 4) return 4;

    /* &arr[k] - arrayName (another ptr - array form, runtime k) */
    if ((int)(&arr[keep(3)] - arr) != 3) return 5;

    /* control: &arr[i] - &arr[j] (both already pointers — the existing c40 path,
     * must STILL be the element count, not bytes) */
    if ((int)(&arr[4] - &arr[1]) != 3) return 6;

    return 42;
}
