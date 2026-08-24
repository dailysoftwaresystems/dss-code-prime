/* FC17.9(d) atomic cycle-1 (D-CSUBSET-ATOMIC) — end-to-end runtime witness for the
 * C11/C23 <stdatomic.h> lock-free scalar surface, RUN on BOTH the baseline (debug)
 * AND the `release` pipeline (Mem2Reg + Inlining active). Three arms, one exit code.
 *
 * A scalar `_Atomic` access lowers to the dedicated MIR AtomicLoad/AtomicStore ops
 * (seq_cst by default, C11 6.5.2.4/7.17.3) which the Phase C per-order LIR fence
 * matrix realizes as REAL fences — x86-64: plain mov loads, `xchg [mem],reg`
 * (implicit-LOCK) for a seq_cst store; arm64: LDAR/STLR (RCsc, no DMB needed),
 * LDR/STR for relaxed. hasSideEffects + opcodeClobbersMemory keep them off every
 * optimizer gate (not promoted / CSE'd / hoisted / DCE'd), so the `release` arm is a
 * RUNTIME witness the atomic round-trips survive the full release pipeline un-elided (a dropped
 * store would leave the load reading an uninitialized slot → a wrong exit).
 *
 * The three arms:
 *   (1) via_atomic         — a plain `_Atomic int` (assignment = AtomicStore seq_cst,
 *                            read = AtomicLoad seq_cst; no explicit order).
 *   (2) via_explicit       — `#include <stdatomic.h>`: an `atomic_int` (a genuinely
 *                            _Atomic-qualified int typedef, M1) accessed with the
 *                            explicit accessors + real memory_order args
 *                            (atomic_store_explicit relaxed + atomic_load_explicit
 *                            seq_cst) — the order folds into the MIR payload.
 *   (3) via_atomic_volatile — `_Atomic volatile int` (the user-named combination):
 *                            lowers to AtomicLoad/AtomicStore ALONE — the atomic op
 *                            subsumes volatile's no-elide/no-CSE/no-hoist guarantees.
 *
 * ANTI-FOLD: g_a/g_b/g_c are mutable globals (runtime-opaque, the atomic_cas/setjmp
 * idiom) so no pass can const-fold the atomic store+load round-trips to `return 42`.
 *
 * RED-on-disable (header/typedef): delete shippedLibs/stdatomic.json → the
 * `#include <stdatomic.h>` fires F_ShippedHeaderNotFound and the compile fails.
 * The differential across x86-64 (xchg/mov) and arm64 (LDAR/STLR) MUST agree.
 *
 * exit = 10 + 12 + 20 = 42.
 */
#include <stdatomic.h>

/* Mutable globals = runtime-opaque operands (defeats const-folding the round-trips). */
int g_a = 10;
int g_b = 12;
int g_c = 20;

/* (1) a plain `_Atomic int` — seq_cst store + load, no explicit order. */
static int via_atomic(int v) {
    _Atomic int a;
    a = v;          /* AtomicStore, payload seq_cst (5) */
    return a;       /* AtomicLoad,  payload seq_cst (5) */
}

/* (2) the <stdatomic.h> explicit accessors with real memory_order args. */
static int via_explicit(int v) {
    atomic_int x;
    atomic_store_explicit(&x, v, memory_order_relaxed);   /* AtomicStore, payload 0 */
    return atomic_load_explicit(&x, memory_order_seq_cst); /* AtomicLoad,  payload 5 */
}

/* (3) `_Atomic volatile int` — the atomic op subsumes volatile (no separate flag). */
static int via_atomic_volatile(int v) {
    _Atomic volatile int av;
    av = v;         /* AtomicStore */
    return av;      /* AtomicLoad  */
}

/* (4) RED-ON-DISABLE for the x86 seq_cst-store XCHG copy-to-scratch (code-audit IMPORTANT):
 *     the STORED value `v` is LIVE-AFTER the store (it is RETURNED, not the load). x86
 *     `xchg [mem],reg` writes the OLD memory value back into `reg`, so WITHOUT the
 *     copy-to-scratch the value register is clobbered and `return v` yields the old slot
 *     (100), not v. g_slot's known old value makes the miscompile deterministic. Correct
 *     (scratch-copy intact): returns g_wit; clobbered (fix removed): returns 100. */
_Atomic int g_slot = 100;   /* the known OLD value the XCHG reads back on a clobber */
int g_wit = 7;              /* the value stored+returned (mutable global = anti-fold) */
static int store_live(int v) {
    g_slot = v;   /* AtomicStore seq_cst -> x86 xchg; v MUST survive the write-back clobber */
    return v;     /* correct: v; a clobbered value reg: 100 (the old g_slot) */
}

int main(void) {
    int a = via_atomic(g_a);             /* 10 */
    int b = via_explicit(g_b);           /* 12 */
    int c = via_atomic_volatile(g_c);    /* 20 */
    int d = store_live(g_wit);           /* g_wit (7) iff the XCHG value survives the clobber */
    return a + b + c + (d - g_wit);      /* 42 + 0; a clobber -> 42 + (100-7) = 135 */
}
