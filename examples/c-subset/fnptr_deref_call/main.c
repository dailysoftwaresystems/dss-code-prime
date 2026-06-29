/* c54 (D-CSUBSET-FUNCTION-POINTER-DEREF): calling through a DEREFERENCED
 * pointer-to-a-function-pointer. SQLite os_unix (sqlite3.c:46575):
 *     typedef const sqlite3_io_methods *(*finder_type)(const char*, unixFile*);
 *     pLockingStyle = (**(finder_type*)pVfs->pAppData)(zFilename, pNew);
 * fired error[S0004] S_NotCallable. checkCall's structural deref-peel descended
 * PAST each `*` and typed the inner node — correct for the function-designator
 * collapse (`*fp == fp` when fp is a fn-ptr, C 6.5.3.2p4) but it DROPPED a real
 * pointer level for `finder_type*` (Ptr<Ptr<FnSig>>), so the callee landed as a
 * pointer-to-fn-pointer and was rejected as not callable. Fix: when the
 * WHOLE-callee type (subtreeType, which applies derefResultType per `*`) is
 * callable, that result governs the triage. The fn-ptr deref + indirect-call
 * lowering was already correct (one data Load of the fn-ptr, no Load through
 * the code pointer) — so a semantic-only fix unblocks it end-to-end.
 *
 * RED-ON-DISABLE: revert the upgrade -> (**(finder_t*)app) and (*pf) land on the
 * pointer-to-fn-pointer -> S0004 S_NotCallable (does not compile).
 *
 * VALUE-CORRECT (each call must hit the right function with the right arg, so a
 * wrong deref level or call target breaks the exact value):
 *   a = (**(finder_t*)app)(2)  — the EXACT SQLite shape: app is a void* aliasing
 *       &f, f = add40; cast void*->finder_t*, double-deref, call -> add40(2)=42.
 *   b = (*pf)(2)               — single-deref through a pointer-to-fn-ptr (the
 *       smaller form the bisection found, also S0004 before) -> add40(2)=42. */
typedef int (*finder_t)(int);

int add40(int x) { return x + 40; }

int main(void) {
    finder_t f = add40;
    finder_t *pf = &f;
    void *app = pf;                    /* like pVfs->pAppData (a void*) */

    int a = (**(finder_t*)app)(2);     /* cast void*->finder_t*, double-deref, call */
    int b = (*pf)(2);                  /* single-deref through finder_t* */

    if (a != 42) return 1;
    if (b != 42) return 2;
    return a;                          /* 42 */
}
