/* c50 (D-CSUBSET-ARRAY-DECAY-TO-VOID-PTR): an ARRAY (or string literal) passed to
 * a `void*` parameter decays to a pointer-to-element (C 6.3.2.1p3) which then
 * converts to void* (C 6.3.2.3p1). The frontier is sqlite's printf engine
 * `memcpy(buf, "-Inf", 5)` (sqlite3VXPrintf) where `buf` is a `char[N]` array —
 * S0003 S_TypeMismatch on BOTH `buf` (the array) and `"-Inf"` (a string literal).
 *
 * ROOT: the isAssignable array-decay arm (type_rules.hpp) only admitted
 * `Array<T> -> Ptr<T>` (same element); `Array<char> -> Ptr<void>` (void != char)
 * fell through, and the Ptr->void arm never fired (the rhs is Array, not Ptr). So
 * `char*`/`u8*` POINTER args — almost every sqlite memcpy — passed via the
 * Ptr->void arm, but a BARE ARRAY name + a string literal (this site) failed.
 *
 * FIX (NOT §B; one disjunct per tier, reusing the existing `implicitToVoidPtr`
 * flag): semantic isAssignable + HIR coerce both admit Array -> Ptr<void> and
 * emit the synthetic Array->Ptr Cast (MIR re-types it by the void* target,
 * element-agnostic). NO MIR / verifier change.
 *
 * RED-ON-DISABLE: revert either arm -> `memcpy(a,"xyz",4)` fails S0003 (does not
 * compile). A `char*` pointer arg stays fine either way (the Ptr->void arm) —
 * proving the gap was the ARRAY/string-literal -> void* decay, not char*->void*. */
#include <string.h>

int main(void) {
    char a[8];
    memset(a, 0, sizeof(a));     /* array a[] -> void* (memset dst) */
    memcpy(a, "xyz", 4);         /* array a[] -> void* AND string-literal "xyz" -> const void* */
    if (a[0] != 'x' || a[1] != 'y' || a[2] != 'z' || a[3] != 0) return 1;
    return 42;
}
