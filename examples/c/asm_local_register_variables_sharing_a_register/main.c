/* TWO LOCAL REGISTER VARIABLES ON ONE REGISTER — ONE AN OPERAND, THE OTHER
 * ASSIGNED AFTER IT — AND THE MEANING DSS KEEPS
 * (D-C-LOCAL-REGISTER-VARIABLES-SHARING-A-REGISTER-MEANING-SPLIT).
 *
 * `a` and `b` are both bound to one register. `a` is assigned 42, then `b` is
 * assigned 4, then an asm statement reads ONLY `a`. The references split on what
 * that means (✔MEASURED, each SEPARATELY at -O0 and -O2): clang 18.1.3 gives 42 —
 * the operand's value is copied into its register AT THE STATEMENT; gcc 13.3.0
 * gives 4 — the variable IS the register, so `b`'s assignment overwrote it.
 *
 * ★ THE RULING (P68 round 8, delegated to the orchestrator by the operator): DSS
 * keeps clang's meaning. gcc's own manual says the ONLY supported use of a
 * local register variable is supplying the operands of an extended asm, and
 * that the register's contents are NOT guaranteed outside that statement — so
 * gcc's 4 depends on the register holding a value BETWEEN statements, a use gcc's
 * documentation itself leaves unsupported. The copy-in meaning is the one both
 * compilers' documentation supports.
 *
 * The template reads the operand through `%1`, so what it sees is exactly what
 * the binding put in the register at the statement. Every seed is `volatile`,
 * so the release pipeline cannot fold either assignment away. Exit 42 = the
 * ruled meaning; 4 = gcc's.
 */
typedef long long reg_t;

#if defined(__aarch64__) || defined(__arm64__)
#define SHARED "x9"
#define READ "mov %0, %1"
#else
#define SHARED "rsi"
#define READ "movq %1, %0"
#endif

volatile reg_t seed_a = 42;
volatile reg_t seed_b = 4;

static reg_t read_a_after_b(void) {
    register reg_t a __asm__(SHARED);
    register reg_t b __asm__(SHARED);
    reg_t r;
    a = seed_a;
    b = seed_b;
    __asm__ volatile (READ : "=r"(r) : "r"(a));
    (void)b;
    return r;
}

reg_t (*volatile fp)(void) = read_a_after_b;

int main(void) { return (int)fp(); }
