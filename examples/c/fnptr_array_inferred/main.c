/* c47 (D-CSUBSET-FNPTR-ARRAY-SIZE-INFERENCE): a `[]` (inferred-size) array of
 * FUNCTION POINTERS with an initializer infers its length from that initializer,
 * exactly like c34's simple-array inference — but the array `[]` lives on the
 * INNER declarator inside the parenthesized group `(*const NAME[])(sig)`, so it
 * is folded by the declarator engine's GROUP-recursion arm. c34 only relaxed the
 * TOP-LEVEL array suffix; the group arm hardcoded allowFlexibleArray=false (the
 * c10 FAM gate), so an inferred fn-ptr-array `[]` wrongly fired
 * S_NonConstantArrayLength. c47 threads an init-inference signal
 * (allowInitInferredArray) that DOES cross into the group (while FAM still does
 * NOT — c10 preserved), so the inner `[]` becomes an incomplete array and is
 * sized once from the brace count (Pass 1.5), like every other inferred array.
 *
 * The frontier is sqlite3.c:186647
 *   static int (*const sqlite3BuiltinExtensions[])(sqlite3*) = { sqlite3Fts5Init, … };
 * — an inferred-size array of const pointers to int(sqlite3*). `tbl` below mirrors
 * the GROUPED inferred declarator as a LOCAL; sqlite's file-scope `static` form
 * ALSO needs c48's global fn-ptr-table lowering (a SEPARATE gap — see main).
 *
 *   tbl = { cb_a, cb_b, cb_c }   inferred  int (*[3])(int)   (local; size 3 = brace count)
 *   i   = pick(2)                a RUNTIME index, opaque across the un-inlined pick()
 *   tbl[i](45) = cb_c(45) = 45 - 5 = 40    (storage-array index, runtime i)
 *   tbl[0](1)  = cb_a(1)  =  1 + 1 =  2
 *   return 40 + 2 == 42
 *
 * RED-ON-DISABLE: revert the group arm to allowFlexibleArray=false (drop the c47
 * init-inference thread) → the inner `[]` fails S_NonConstantArrayLength (S000B)
 * → does not compile. A WRONG inferred size lands tbl[2] off-storage and the
 * exit is not 42. The release / optimized arm keeps `i` a runtime value across
 * pick() so the indexed indirect-call path runs (no whole-expr const-fold).
 * (A no-init `int (*NAME[])(int);` and a fn-ptr PARAM `int (*f)(int x[])` both
 * KEEP the ordinary S000B / decay behavior — c10 unchanged — per the audit.) */

int cb_a(int x) { return x + 1; }
int cb_b(int x) { return x * 2; }
int cb_c(int x) { return x - 5; }

int pick(int x) { return x; }

int main(void) {
    /* inferred-size LOCAL array of pointers to int(int) — the grouped declarator
     * `(*tbl[])(int)`, sized from the brace count. LOCAL (not the sqlite file-scope
     * `static`) so the aggregate init lowers into a stack slot — a GLOBAL fn-ptr
     * table of function-address relocations is a SEPARATE lowering gap (c48). */
    int (*tbl[])(int) = { cb_a, cb_b, cb_c };
    int i = pick(2);
    return tbl[i](45) + tbl[0](1);   /* cb_c(45) + cb_a(1) == 40 + 2 == 42 */
}
