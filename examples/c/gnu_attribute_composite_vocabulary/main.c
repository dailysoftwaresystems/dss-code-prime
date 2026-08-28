/* P42 — D-CSUBSET-GNU-UNKNOWN-NAME-GATE-ASYMMETRY (the composite half).
 *
 * WHAT THIS EXAMPLE WITNESSES, and it is an ACCEPTANCE change before it is a
 * layout one: three GNU attributes that this language's own
 * `attributeSemantics.effects` table declares KNOWN were REFUSED on a type
 * definition, because the composite attribute scan decided "unknown" from its
 * own three consumed-here arms instead of from the shared clause reader's
 * already-resolved effects row — one chokepoint knowing the answer while a
 * second site restated a drifted roster.
 *
 * ✔MEASURED with the shipped CLI before the fix, every one exit 1:
 *     struct __attribute__((may_alias))         S { … };   error[S0031]
 *     struct __attribute__((unused))            S { … };   error[S0031]
 *     union  __attribute__((transparent_union)) U { … };   error[S0031]
 * ✔MEASURED against the references, each probed separately — gcc 13.3.0
 * (`-std=c2x`) and clang 18.1.3 (`-std=c23`), both with `-Wall -Wextra`: all
 * three compile with ZERO diagnostics and exit 0 on BOTH. DSS was refusing
 * programs every real toolchain builds, which is why no artifact existed to run
 * and why this example could not have been written before.
 *
 * THE CHECKS ARE LAYOUT, NOT COMPILATION. "It compiled" is a weak witness for a
 * type attribute: the failure mode of a composite attribute scan is a struct of
 * the wrong size, which emits nothing. Each check therefore reads a `sizeof` and
 * exits its own code, so a failure names itself:
 *
 *     struct MA (may_alias)         sizeof 8  -> exit 1
 *     struct UU (unused)            sizeof 4  -> exit 2
 *     union  TU (transparent_union) sizeof 4  -> exit 3
 *     struct PK (packed)            sizeof 5  -> exit 4
 *     all correct                             -> exit 42
 *
 * ★ `PK` IS THE CONTROL, AND IT IS THE HALF THAT MAKES THE OTHER THREE WORTH
 * ANYTHING. `packed` is consumed by an arm ABOVE the one this cycle changed, so
 * widening which names count as KNOWN must not cost it its LAYOUT effect. If the
 * widening had swallowed it, `sizeof(struct PK)` would be 8 rather than 5 —
 * wrong bytes, caught here at runtime rather than by a diagnostic count.
 *
 * ★ THE `release` ARM IS MANDATORY, not decoration: these sizes are folded at
 * compile time, so the point of the optimized arm is witnessing that the SHIPPED
 * pipeline preserves the composite layouts rather than re-deriving them.
 *
 * The whole file is VERIFIED clean on gcc 13.3.0 and clang 18.1.3
 * (`-std=c2x` / `-std=c23`, `-Wall -Wextra`): zero errors, zero warnings, and
 * both reference-built binaries independently exit 42 — so the expected exit
 * code is ground truth from a real toolchain, not merely what DSS happens to do.
 */

struct __attribute__((may_alias)) MA {
    int  a;
    char b;
};

struct __attribute__((unused)) UU {
    int a;
};

union __attribute__((transparent_union)) TU {
    int a;
};

struct __attribute__((packed)) PK {
    int  a;
    char b;
};

/* Kept out of the constant folder's reach so the optimized arm has real work:
 * the accumulator is only ever read after the loop. */
static int sum_to(int n) {
    int acc = 0;
    for (int i = 0; i < n; ++i) acc += i;
    return acc;
}

int main(void) {
    struct MA ma;
    struct UU uu;
    union  TU tu;
    struct PK pk;

    ma.a = 1;
    ma.b = 2;
    uu.a = 3;
    tu.a = 4;
    pk.a = 5;
    pk.b = 6;

    if (sizeof(struct MA) != 8u) return 1;
    if (sizeof(struct UU) != 4u) return 2;
    if (sizeof(union  TU) != 4u) return 3;
    if (sizeof(struct PK) != 5u) return 4;

    /* Read every object back so no declaration can be optimized away without the
     * layout having been real. 1+2+3+4+5+6 = 21, and sum_to(7) = 21. */
    if (ma.a + ma.b + uu.a + tu.a + pk.a + pk.b != 21) return 5;
    if (sum_to(7) != 21) return 6;

    return 42;
}
