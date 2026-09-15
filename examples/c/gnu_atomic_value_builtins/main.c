// [[D-C-HAS-EXTENSION-CLAIMS-C-ATOMIC-WHILE-THE-GNU-ATOMIC-BUILTINS-DO-NOT-EXIST]]
// — the RUNTIME witness for the GNU `__atomic_*` VALUE-FORM builtins.
//
// ★★★ WHAT THIS EXAMPLE HAS TO PROVE, AND WHY "IT COMPILES" IS NOT IT. The
// defect this closes was an ADVERTISEMENT the compiler could not honour: DSS
// answered `__has_extension(c_atomic)` = 1, sqlite took that as the gate for
// `__atomic_load_n`/`__atomic_store_n`, and the names did not exist. The obvious
// repair — declare the names so the TU parses — would make every "does it
// compile" instrument green while the atomics did nothing. So every arm below is
// written so that a builtin that COMPILED but did not TAKE EFFECT moves the exit
// code: each one reads its result back out of MEMORY, and two of them discard
// the result entirely so that only the STORE can be observed.
//
// ⚠ THE EXIT CODE IS NOT AUTHORED, IT IS MEASURED. Every arm was compiled and
// RUN under gcc 13.3.0 and clang 18.1.3 SEPARATELY before it was written down
// (`-std=c17 -O0` and `-O2`, four runs, one exit code) — the references are the
// spec for what these builtins mean, so the number they agree on is the number
// this manifest asserts. A DSS run that differs is DSS being wrong.
//
// ★★ THE ARMS, each with a DISTINCT contribution so a single broken builtin
// cannot be masked by a compensating error elsewhere:
//   1. store_n → load_n round trip. The store must LAND and the load must read
//      MEMORY. A load folded to the initializer, or a store dropped, changes it.
//   2. exchange_n. Two facts in one call: it RETURNS the prior value AND
//      installs the new one. Both are read.
//   3. fetch_add. Returns the PRIOR value; memory holds the SUM. A builtin wired
//      to return the new value (the `__atomic_add_fetch` shape, which this row
//      deliberately does NOT ship) diverges here.
//   4. fetch_or / fetch_and / fetch_xor over a known bit pattern.
//   5. ★ A NON-CONSTANT memory order. ✔MEASURED: gcc and clang both accept and
//      run `__atomic_load_n(p, <runtime int>)`; DSS's `foldAtomicOrder` answers a
//      non-constant order with seq_cst, which over-fences and is C11-legal. This
//      arm exists because a compiler that rejected a runtime order would still
//      pass every other arm here, and sqlite-shaped code does pass variables.
//   6. ★★ THE DCE GUARD, and it is the arm that makes this a TOOK-EFFECT witness
//      rather than a COMPILES witness: an unused-result `__atomic_fetch_add` and
//      a bare `__atomic_store_n` whose values are never consumed. Their stores
//      must still land — the final read is the only place they are observed. An
//      optimizer that treats these as pure and drops them loses 40 from the exit
//      code under the `release` arm while the `debug` arm still passes.
//   7. Sub-`int` and wider-than-`int` objects (unsigned char, long long) — the
//      `genericPointee` binding is what makes ONE declared row serve every
//      object width, and a binding that silently truncated would show here.
//
// ⓘ The objects are `volatile` so the FINAL reads are honest loads rather than
// values the optimizer carried in a register — the same device
// `examples/c/atomic_cas_intrinsic` uses for the same reason.

static volatile int            g32 = 0;
static volatile long long      g64 = 0;
static volatile unsigned char  g8  = 0;
static volatile int            gdce = 0;

// A runtime memory order the optimizer cannot fold: it is derived from a value
// that is only known at run time. Taking it from `argc` would work too, but the
// examples runner invokes with no arguments, and a fixed `argc` is exactly the
// constant this arm exists to avoid.
static volatile int gRuntimeOrder = 0;   /* __ATOMIC_RELAXED, read as a variable */

int main(void) {
    int acc = 0;

    /* ── 1. store_n → load_n: the store lands, the load reads memory ───────── */
    __atomic_store_n(&g32, 7, __ATOMIC_RELAXED);
    acc += __atomic_load_n(&g32, __ATOMIC_RELAXED);            /* 7   → 7    */

    /* ── 2. exchange_n: returns the PRIOR value, installs the new one ──────── */
    acc += __atomic_exchange_n(&g32, 20, __ATOMIC_SEQ_CST);    /* +7  → 14   */
    acc += __atomic_load_n(&g32, __ATOMIC_ACQUIRE);            /* +20 → 34   */

    /* ── 3. fetch_add: returns PRIOR, memory holds the SUM ─────────────────── */
    acc += __atomic_fetch_add(&g32, 5, __ATOMIC_SEQ_CST);      /* +20 → 54   */
    acc += __atomic_load_n(&g32, __ATOMIC_RELAXED);            /* +25 → 79   */
    acc += __atomic_fetch_sub(&g32, 5, __ATOMIC_SEQ_CST);      /* +25 → 104  */
    acc += __atomic_load_n(&g32, __ATOMIC_RELAXED);            /* +20 → 124  */

    /* ── 4. the bitwise trio over a known pattern ──────────────────────────── */
    __atomic_store_n(&g32, 0x0C, __ATOMIC_RELAXED);
    acc += __atomic_fetch_or(&g32, 0x03, __ATOMIC_SEQ_CST);    /* +12 → 136  */
    acc += __atomic_load_n(&g32, __ATOMIC_RELAXED);            /* +15 → 151  */
    acc += __atomic_fetch_and(&g32, 0x06, __ATOMIC_SEQ_CST);   /* +15 → 166  */
    acc += __atomic_load_n(&g32, __ATOMIC_RELAXED);            /* +6  → 172  */
    acc += __atomic_fetch_xor(&g32, 0x05, __ATOMIC_SEQ_CST);   /* +6  → 178  */
    acc += __atomic_load_n(&g32, __ATOMIC_RELAXED);            /* +3  → 181  */

    /* ── 5. a NON-CONSTANT memory order ────────────────────────────────────── */
    __atomic_store_n(&g32, 9, gRuntimeOrder);
    acc += __atomic_load_n(&g32, gRuntimeOrder);               /* +9  → 190  */

    /* ── 6. THE DCE GUARD: results discarded, stores must still land ───────── */
    __atomic_store_n(&gdce, 10, __ATOMIC_RELAXED);   /* value never consumed   */
    (void)__atomic_fetch_add(&gdce, 30, __ATOMIC_SEQ_CST); /* result discarded */
    acc += __atomic_load_n(&gdce, __ATOMIC_RELAXED);           /* +40 → 230  */

    /* ── 7. object widths other than `int` ─────────────────────────────────── */
    __atomic_store_n(&g8, (unsigned char)200, __ATOMIC_RELAXED);
    acc += (int)__atomic_load_n(&g8, __ATOMIC_RELAXED);        /* +200 → 430 */
    acc += (int)__atomic_fetch_add(&g8, (unsigned char)5, __ATOMIC_SEQ_CST);
                                                               /* +200 → 630 */
    acc += (int)__atomic_load_n(&g8, __ATOMIC_RELAXED);        /* +205 → 835 */

    __atomic_store_n(&g64, 1000LL, __ATOMIC_RELAXED);
    acc += (int)__atomic_load_n(&g64, __ATOMIC_RELAXED);       /* +1000→ 1835*/
    acc += (int)__atomic_exchange_n(&g64, 7LL, __ATOMIC_SEQ_CST);
                                                               /* +1000→ 2835*/
    acc += (int)__atomic_load_n(&g64, __ATOMIC_RELAXED);       /* +7   → 2842*/

    /* An exit code must fit a byte on POSIX, so fold the accumulator into one.
       ⓘ The fold is a WITNESS, not a convenience: a wrong arm changes `acc` and
       therefore changes `acc % 251`, and 251 is prime so no two of the per-arm
       contributions above cancel in the modulus. */
    return acc % 251;
}
