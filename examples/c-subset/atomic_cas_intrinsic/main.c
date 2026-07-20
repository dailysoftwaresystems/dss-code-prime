// c104 (D-CSUBSET-INTRINSIC-ATOMIC-CAS): end-to-end runtime witness for the
// `_InterlockedCompareExchange` compiler intrinsic -- the atomic 32-bit
// compare-and-swap (returns the ORIGINAL value; stores iff original==comparand).
// A config-declared builtin (c-subset.lang.json, lowering:"atomic_cas") lowering
// to x86 `lock cmpxchg [mem],reg` (implicit-RAX comparand/old) or an arm64
// LDAXR/STLXR acquire-release retry LOOP (real CFG blocks -- the qemu arm64 arm
// of this example is the runtime proof the loop executes correctly). NOT a call
// or import (an intrinsic has no linkable symbol). The Windows-SDK-shaped macro
// `InterlockedCompareExchange` -> `_InterlockedCompareExchange` (windows.json)
// is witnessed by the sqlite pe64 probe; this corpus pins the intrinsic itself
// on every target, so the CAS target is `int volatile` (i32 on ALL data models
// -- Win32 LONG is i32 only under LLP64).
//
// THREE arms, each with a DISTINCT exit-code contribution:
//   1. SUCCESS: x==5, CAS(&x, new=9, comp=5) -> returns 5 (the original), x
//      becomes 9. A swapped comparand/newval wiring or a wrong 'old' capture
//      changes r1 AND x.
//   2. FAILURE: x==9 now, CAS(&x, new=77, comp=5) -> 9 != 5, so NO store: x
//      stays 9 and the return is the OBSERVED 9 (not 5, not 77). A CAS that
//      stores on inequality (inverted condition) or returns the comparand
//      instead of the observed value diverges here.
//   3. UNUSED-RESULT (the hasSideEffects/DCE guard): y==11, the CAS's result is
//      DISCARDED -- the STORE must still land (y becomes 23). A DCE that drops
//      a "dead" CAS (treating it as pure like UMulH) loses the store and the
//      final y read returns 11, shifting the exit by -12.
// volatile globals keep the final x/y reads honest loads AND make the &x args
// the exact sqlite winMutex shape (`LONG volatile winMutex_lock`):
// Ptr<volatile CasWord> coercing to the builtin's declared pointee.
//
// D-LANG-TYPE-IDENTITY-VOCABULARY: the CAS WORD is now spelled per platform.
// The Windows SDK declares the intrinsic `_InterlockedCompareExchange(LONG
// volatile*, LONG, LONG)` and `LONG` IS `long` — a 32-bit `long` under LLP64.
// Since type identity is no longer derived from representation, `int*` and
// `long*` are DISTINCT types even at one width, so passing an `int volatile*`
// where the SDK says `long volatile*` is the same constraint violation a real
// C compiler diagnoses. `_WIN32` is pe-gated (c-subset.lang.json predefines),
// so this stays i32 on EVERY target (Win32 `long` is 32-bit; elsewhere the
// builtin's own base signature is the anonymous `int*`) — the example's
// all-targets i32 CAS shape is unchanged, only its SPELLING is now honest.
//
// exit = r1 + r2 + x + y = 5 + 9 + 9 + 23 = 46.

#if defined(_WIN32)
typedef long CasWord;    /* Win32 LONG — 32-bit under LLP64 */
#else
typedef int  CasWord;
#endif

static CasWord volatile x = 5;
static CasWord volatile y = 11;

int main(void) {
    CasWord r1 = _InterlockedCompareExchange(&x, 9, 5);   /* success: 5, x->9   */
    CasWord r2 = _InterlockedCompareExchange(&x, 77, 5);  /* failure: 9, x stays */
    (void)_InterlockedCompareExchange(&y, 23, 11);        /* unused result: y->23 */
    return (int)(r1 + r2 + x + y);
}
