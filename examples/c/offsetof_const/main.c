/* c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD / Option A) — `offsetof(T,M)` as a
 * COMPILE-TIME constant. C requires `offsetof` (the portable form
 * `((size_t)((char*)&((T*)0)->M - (char*)0))`) to be an integer constant
 * expression, so it may size an array. DSS's const-evaluator was integer-only and
 * could not fold the address spine `(char*)&((T*)0)->M - (char*)0`, so an array
 * dimension using it degraded to an incomplete array → S000B (plain) / S001C
 * (sole struct member). The fix adds a null-relative ADDRESS-CONSTANT value kind
 * to the evaluator: `(T*)0` → {base=NULL, offset=0}; `->M` adds the field's layout
 * offset; `&` is a no-op over the lvalue address; `(char*)` retypes; the final
 * `- (char*)0` collapses to the integer byte offset. This is the EXACT sqlite
 * shape (`struct uKey { u8 keyinfoSpace[SZ_KEYINFO_0]; }` where SZ_KEYINFO_0 =
 * offsetof(KeyInfo, aColl), sqlite3.c:24984).
 *
 * Value-CORRECT, not just "compiles": the offsets are read from the per-target
 * layout engine, so alignment padding is honored (A.d is at 8, NOT 1). Each case
 * sizes a SOLE-member char array by an offsetof (the sqlite shape) and checks its
 * sizeof == the true offset. Exit 42 iff every offset folds correctly.
 *
 * RED-ON-DISABLE: revert the c43 const-eval address arms → the offsetof dimension
 * is non-constant → the sole-member struct is an incomplete-array FAM → S001C
 * (would not compile). */
typedef unsigned long size_t;         /* the cast type sqlite's offsetof terminates with */

struct A { char c; double d; };       /* d at offset 8 — alignment padding */
struct B { int a; int b; char c; };   /* c at offset 8 */
struct K { int n; int m; };           /* m at offset 4 — the sqlite KeyInfo shape */

/* sole-member structs sized by offsetof (the `struct uKey` pattern). The first
 * three use the `(unsigned long)` keyword-cast terminator; PadSz uses the
 * CANONICAL sqlite `(size_t)` typedef-cast terminator (size_t is a defined
 * typedef here, exactly as in real C via <stddef.h>) — both fold identically. */
struct PadA  { char x[ ((unsigned long)((char*)&((struct A*)0)->d - (char*)0)) ]; };
struct PadB  { char x[ ((unsigned long)((char*)&((struct B*)0)->c - (char*)0)) ]; };
struct PadK  { char x[ ((unsigned long)((char*)&((struct K*)0)->m - (char*)0)) ]; };
struct PadSz { char x[ ((size_t)      ((char*)&((struct K*)0)->m - (char*)0)) ]; };

int main(void) {
    if (sizeof(struct PadA)  != 8) return 1;   /* A.d at 8 (alignment, not 1) */
    if (sizeof(struct PadB)  != 8) return 2;   /* B.c at 8 */
    if (sizeof(struct PadK)  != 4) return 3;   /* K.m at 4 (the sqlite shape) */
    if (sizeof(struct PadSz) != 4) return 4;   /* K.m at 4 via the (size_t) form */
    return 42;
}
