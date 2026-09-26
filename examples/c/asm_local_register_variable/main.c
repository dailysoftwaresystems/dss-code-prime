/* GNU LOCAL REGISTER VARIABLES — `register reg_t v __asm__("r10") = 40;` — and
 * the inline-asm operand that reads or writes one, end to end.
 * D-C-LOCAL-REGISTER-VARIABLE-ASM-LABEL-IGNORED,
 * D-ASM-MATCHING-CONSTRAINT-DIGIT-READ-AS-A-MACHINE-LETTER.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. 📄 GCC manual, "Local Register Variables": the
 * only supported use of `register T v asm("reg")` is to specify registers for
 * the input and output operands of an extended `asm` — a register-form operand
 * whose value IS the variable is bound to that register. ✔MEASURED 2026-09-19 at
 * the P68 round-7 base, through the real CLI: the label was IGNORED (the
 * automatic-variable warn-and-drop), each binding shape below compiled ALONE at
 * rc=0 with its value wherever the allocator put it — the wrong answer at debug
 * AND release unless the allocator happened to choose the named register — and
 * this file as a whole was refused, because the matching constraint `"0"` was
 * read as a machine letter.
 *
 * ★★ THE REFERENCES, each probed SEPARATELY at -O0 and -O2 (x86_64 natively,
 * aarch64 under qemu): gcc 13.3.0 runs the whole file to 42 on both. clang
 * 18.1.3 refuses x86_64 shape E ("couldn't allocate input reg for constraint
 * '{rax}'" — it binds no two-register value on x86_64) and, on aarch64 at -O0,
 * dies in shape F (it copies the never-written variable's indeterminate stack
 * slot INTO sp; ✔MEASURED by removing one shape at a time — only F's removal
 * turns the crash into 42); aarch64 -O2 runs it to 42. Reading an object that
 * was never written is undefined (C 6.3.2.1p2), and gcc's meaning — the
 * variable IS its register — is the one this compiler implements.
 *
 * ★★ EVERY TEMPLATE NAMES ITS REGISTER BY NAME, NEVER BY `%N`. That is what
 * makes the binding observable: a template reading `%1` would read whatever
 * register the operand got and pass whether or not the binding held. Reading
 * `x9`/`r10` by name passes only if the value is really there.
 *
 * ★ `reg_t` IS `long long`, NOT `long`: Windows x86_64 is LLP64, where `long` is
 * 32 bits and a `leaq … , %0` on it names a 32-bit register (✔MEASURED, the PE
 * arm refused exactly that). Every shape has its own exit code, so a failure
 * names the shape, and every seed is `volatile`, so the release pipeline still
 * has to move a value into the bound register rather than fold the statement.
 *
 * SHAPES:
 *   A (11) an input bound to its register
 *   B (12) an output bound to its register
 *   C (13) a `+` operand bound to its register (read and written)
 *   D (14) a GNU matching constraint `"0"`: the input shares its output's register
 *   E (15) a 16-byte value on one bound register continues in the target's
 *          declared next register (x4:x5 / rax:rdx)
 *   F (16) a variable NEVER written, read on an input: the register AS IT STANDS
 *          (the stack pointer — nothing may be copied into it)
 *   G (17) a CALLEE-SAVED register bound in a called function (the binding makes
 *          the function save and restore it — `noinline`, so it is a real frame;
 *          that a caller's value SURVIVES is allocation-dependent under a
 *          copy-at-the-operand binding and is pinned in the unit test instead)
 *   H (18) aarch64 only: the LINK register bound in a leaf — the leaf must save
 *          it, or its `ret` jumps to the bound value
 */

typedef long long reg_t;
typedef unsigned long long ureg_t;

volatile reg_t dss_seed40 = 40;
volatile reg_t dss_seed2 = 2;

#if defined(__x86_64__)

static reg_t shapeA(void) {
    register reg_t v __asm__("r10") = dss_seed40;
    reg_t r;
    __asm__ volatile("leaq 2(%%r10), %0" : "=r"(r) : "r"(v));
    return r;
}

static reg_t shapeB(void) {
    register reg_t v __asm__("r11");
    __asm__ volatile("movq $42, %%r11" : "=r"(v));
    return v;
}

static reg_t shapeC(void) {
    register reg_t v __asm__("r12") = dss_seed40;
    __asm__ volatile("addq $2, %%r12" : "+r"(v));
    return v;
}

static reg_t shapeD(void) {
    register reg_t p1 __asm__("rax") = dss_seed40;
    register reg_t p2 __asm__("rdx") = dss_seed2;
    register reg_t res __asm__("rax");
    __asm__ volatile("addq %%rdx, %%rax" : "=r"(res) : "0"(p1), "r"(p2));
    return res;
}

static reg_t shapeE(void) {
    register __int128 v __asm__("rax") = ((__int128)dss_seed2 << 64) | dss_seed40;
    reg_t r;
    __asm__ volatile("leaq (%%rax,%%rdx), %0" : "=r"(r) : "r"(v));
    return r;
}

static int shapeF(void) {
    register ureg_t sp __asm__("rsp");
    ureg_t r;
    __asm__ volatile("movq %1, %0" : "=r"(r) : "r"(sp));
    ureg_t here = (ureg_t)&r;
    return (r != 0 && r <= here + 4096 && here - r < 65536) ? 42 : 0;
}

__attribute__((noinline)) static reg_t shapeG(reg_t k) {
    register reg_t v __asm__("rbx") = k;
    reg_t r;
    __asm__ volatile("leaq 2(%%rbx), %0" : "=r"(r) : "r"(v));
    return r;
}

#elif defined(__aarch64__)

static reg_t shapeA(void) {
    register reg_t v __asm__("x9") = dss_seed40;
    reg_t r;
    __asm__ volatile("add %0, x9, #2" : "=r"(r) : "r"(v));
    return r;
}

static reg_t shapeB(void) {
    register reg_t v __asm__("x10");
    __asm__ volatile("mov x10, #42" : "=r"(v));
    return v;
}

static reg_t shapeC(void) {
    register reg_t v __asm__("x11") = dss_seed40;
    __asm__ volatile("add x11, x11, #2" : "+r"(v));
    return v;
}

static reg_t shapeD(void) {
    register reg_t p1 __asm__("x0") = dss_seed40;
    register reg_t p2 __asm__("x1") = dss_seed2;
    register reg_t res __asm__("x0");
    __asm__ volatile("add x0, x0, x1" : "=r"(res) : "0"(p1), "r"(p2));
    return res;
}

static reg_t shapeE(void) {
    register __int128 v __asm__("x4") = ((__int128)dss_seed2 << 64) | dss_seed40;
    reg_t r;
    __asm__ volatile("add %0, x4, x5" : "=r"(r) : "r"(v));
    return r;
}

static int shapeF(void) {
    register ureg_t sp __asm__("sp");
    ureg_t r;
    __asm__ volatile("mov %0, %1" : "=r"(r) : "r"(sp));
    ureg_t here = (ureg_t)&r;
    return (r != 0 && r <= here + 4096 && here - r < 65536) ? 42 : 0;
}

__attribute__((noinline)) static reg_t shapeG(reg_t k) {
    register reg_t v __asm__("x19") = k;
    reg_t r;
    __asm__ volatile("add %0, x19, #2" : "=r"(r) : "r"(v));
    return r;
}

__attribute__((noinline)) static reg_t linkRegisterLeaf(reg_t k) {
    register reg_t v __asm__("x30") = k;
    reg_t r;
    __asm__ volatile("add %0, x30, #2" : "=r"(r) : "r"(v));
    return r;
}

#else
#error "asm_local_register_variable: no arm for this architecture — add one rather than \
letting the example pass without binding a register"
#endif

int main(void) {
    if (shapeA() != 42) return 11;
    if (shapeB() != 42) return 12;
    if (shapeC() != 42) return 13;
    if (shapeD() != 42) return 14;
    if (shapeE() != 42) return 15;
    if (shapeF() != 42) return 16;
    if (shapeG(dss_seed40) != 42) return 17;
#if defined(__aarch64__)
    if (linkRegisterLeaf(dss_seed40) != 42) return 18;
#endif
    return 42;
}
