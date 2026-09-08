/* D-CSUBSET-ATOMIC-RMW — the C11/C23 §7.17.7 read-modify-write family, end to
 * end, RUN on BOTH the baseline (debug) and the `release` pipeline.
 *
 * WHAT IS BEING WITNESSED. Each `atomic_fetch_*` / `atomic_exchange` lowers to a
 * HIR→MIR retry loop over the already-shipped `MirOpcode::AtomicCas`:
 *
 *     retry: old = AtomicLoad(obj); new = old <alu> operand
 *            prev = AtomicCas(obj, old, new); if (prev != old) goto retry
 *
 * which is INDIVISIBLE by construction — the CAS commits only if the location
 * still holds the value that iteration read, so any interleaved write discards
 * the iteration. `atomic_compare_exchange_{strong,weak}` need no loop: they ARE
 * the AtomicCas, plus C's `_Bool` result and the failure-only `*expected =
 * observed` side effect. That last half is the one a positional pass-through
 * silently drops, and dropping it is not a compile error anywhere downstream —
 * a caller looping on `while (!atomic_compare_exchange_weak(&o, &e, f(e)))`
 * would spin forever on a stale `e`. Arms 8 and 9 pin exactly that.
 *
 * THE TWO REALIZATIONS DIFFER AND MUST AGREE. x86-64 takes `lock cmpxchg` (a
 * single locked op, a full barrier); arm64 takes a REAL CFG LL/SC retry loop
 * over `ldaxr`/`stlxr`. An x86-only witness would say nothing about the
 * exclusive-pair path, which is where the spill hazard
 * [[D-LIR-LLSC-SPILL-EXCLUSION]] describes lives — so this example runs on both.
 *
 * ANTI-FOLD: every object is a MUTABLE GLOBAL (runtime-opaque), so no pass can
 * const-fold the round trips to `return 42`. The atomic ops carry
 * hasSideEffects + opcodeClobbersMemory, so the `release` arm is a RUNTIME
 * witness that the family survives Mem2Reg/CSE/LICM/DCE/Inlining un-elided.
 *
 * RED-ON-DISABLE: delete the `atomic_fetch_add_explicit` row from
 * `sources/c.lang.json` `builtinFunctions` → `atomic_fetch_add` no longer
 * resolves and the compile fails `S_UndeclaredIdentifier`. Delete the
 * `atomic_fetch_add` macro from `shippedLibs/stdatomic.json` → the same, at the
 * user-facing spelling.
 *
 * Every arm has its OWN non-42 exit, so a failure names itself. exit = 42.
 */
#include <stdatomic.h>

atomic_int g_add = 40;
atomic_int g_sub = 50;
atomic_int g_or  = 0xF0;
atomic_int g_and = 0xF0;
atomic_int g_xor = 0xF0;
atomic_int g_xchg = 7;
atomic_int g_cas  = 11;

/* An RMW whose OLD value is live-after: the family returns the value the object
 * held BEFORE the operation (C §7.17.7.5), not the new one. Returning `new`
 * would still exit 42 on the `_or` arm alone, which is why every arm below
 * checks BOTH the returned old value and the resulting stored value. */
static int fetch_add_returns_old(void) {
    int const old = atomic_fetch_add(&g_add, 2);
    return (old == 40 && atomic_load(&g_add) == 42) ? 0 : 1;
}

int main(void) {
    if (fetch_add_returns_old() != 0)                    return 1;

    if (atomic_fetch_sub(&g_sub, 8) != 50)               return 2;
    if (atomic_load(&g_sub) != 42)                       return 3;

    if (atomic_fetch_or(&g_or, 0x0F) != 0xF0)            return 4;
    if (atomic_load(&g_or) != 0xFF)                      return 5;

    if (atomic_fetch_and(&g_and, 0x30) != 0xF0)          return 6;
    if (atomic_load(&g_and) != 0x30)                     return 7;

    if (atomic_fetch_xor(&g_xor, 0xFF) != 0xF0)          return 8;
    if (atomic_load(&g_xor) != 0x0F)                     return 9;

    if (atomic_exchange(&g_xchg, 99) != 7)               return 10;
    if (atomic_load(&g_xchg) != 99)                      return 11;

    /* compare-exchange, SUCCESS: the comparand matches, the exchange happens,
     * and `expected` is left alone. */
    int expected = 11;
    if (!atomic_compare_exchange_strong(&g_cas, &expected, 42)) return 12;
    if (atomic_load(&g_cas) != 42)                       return 13;
    if (expected != 11)                                  return 14;

    /* compare-exchange, FAILURE: the comparand no longer matches, so NO
     * exchange happens AND `*expected` is overwritten with the value actually
     * observed. The second half is the silent one. */
    expected = 11;
    if (atomic_compare_exchange_strong(&g_cas, &expected, 77)) return 15;
    if (expected != 42)                                  return 16;
    if (atomic_load(&g_cas) != 42)                       return 17;

    /* the weak spelling: a strong CAS is a conforming realization of it. */
    if (!atomic_compare_exchange_weak(&g_cas, &expected, 43)) return 18;
    if (atomic_load(&g_cas) != 43)                       return 19;

    /* the explicit-order spellings reach the same lowering; a weaker requested
     * order is legally over-fenced, never under-fenced. */
    if (atomic_fetch_add_explicit(&g_cas, -1, memory_order_relaxed) != 43)
                                                         return 20;
    if (atomic_load_explicit(&g_cas, memory_order_acquire) != 42)
                                                         return 21;
    return 42;
}
