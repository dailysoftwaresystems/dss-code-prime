/* c113 (D-CSUBSET-INTRINSIC-BARRIER): the <intrin.h> + _ReadWriteBarrier witness.
 *
 * Dropping -DSQLITE_DISABLE_INTRINSIC activates sqlite's `#include <intrin.h>`
 * block and `sqlite3MemoryBarrier`'s `_ReadWriteBarrier()` (the `MSVC_VERSION`
 * branch). This proves DSS (a) resolves the pe-gated <intrin.h> shipped-lib
 * descriptor for the include, and (b) lowers the MSVC COMPILER-barrier intrinsic:
 * `_ReadWriteBarrier` is an always-on builtin (like `__umulh`) that lowers to a
 * zero-operand, side-effecting MIR op (`CompilerBarrier`) which emits NO runtime
 * instruction but forbids the optimizer from moving memory accesses across it.
 *
 * Two stores to a global straddle two barriers, then a load. A single-threaded
 * program cannot OBSERVE compiler reordering, so this returns 42 either way — it
 * witnesses the COMPILE+RUN chain (the include resolves, the builtin lowers, the
 * binary runs), including under the `release` optimizer arm (the barrier must
 * survive DCE/CSE/LICM). The ordering CONTRACT (a load is not hoisted across the
 * fence) is pinned structurally in the MIR tests, where it is observable.
 *
 * RED-on-disable: remove the `_ReadWriteBarrier` builtin (c-subset.lang.json) ->
 * S0001 undeclared; or remove shippedLibs/intrin.json -> F_ShippedHeaderNotFound
 * on the `#include <intrin.h>`. pe64-ONLY: <intrin.h> is a Microsoft header,
 * gated `availableObjectFormats:["pe"]` (elf/macho reject the include).
 */
#include <intrin.h>

static int g;

int main(void) {
    g = 20;
    _ReadWriteBarrier();
    g = 22;
    _ReadWriteBarrier();
    return g + 20;   /* 22 + 20 = 42 */
}
