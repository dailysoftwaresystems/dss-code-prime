/* A LINUX SYSCALL WRAPPER — the shape every libc builds on: GNU local register
 * variables bound to the kernel's argument registers around an inline `syscall`
 * (x86_64) or `svc #0` (aarch64). D-C-LOCAL-REGISTER-VARIABLE-ASM-LABEL-IGNORED,
 * D-ASM-DIALECT-GAPS-A-REFERENCE-ASSEMBLER-ACCEPTS.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. musl's `__syscallN` and glibc's
 * `INTERNAL_SYSCALL_RAW` are exactly this code: `register long x8 asm("x8") = n;`
 * (aarch64) or `register long r10 asm("r10") = a4;` (x86_64, where the fourth
 * argument cannot be named by any constraint letter). ✔MEASURED 2026-09-19 at
 * the P68 round-7 base, through the real CLI: this file was REFUSED — neither
 * dialect spelled the kernel transition (`unknown mnemonic 'syscall'` /
 * `'svc'`) — and the labels were IGNORED, so the binding shapes alone compiled
 * rc=0 with the number and the arguments in arbitrary registers. Once `svc` was
 * spelled, the first build still refused it (`'svc' writes to a destination that
 * is neither a register nor a memory reference`). gcc 13.3.0 and clang 18.1.3
 * run this program to 42 at -O0 and -O2 on x86_64 natively and on aarch64 under
 * qemu.
 *
 * ★★ EVERY CHECK DISCRIMINATES THE BINDING, because a syscall whose number or
 * arguments land in the wrong register returns something else:
 *   11  getpid twice — the same positive value (a wrong NUMBER register runs
 *       another call or -ENOSYS);
 *   12  write(-1, buf, 1) returns -EBADF (-9) — the FIRST argument must be
 *       exactly -1 in its register;
 *   13  rt_sigprocmask(SIG_BLOCK, NULL, &old, 8) returns 0 — the FOURTH
 *       argument (x86_64 r10, aarch64 x3) must be the size 8, and anything else
 *       is -EINVAL;
 *   exit(42) through the raw `exit` call — a wrong number or argument returns
 *       here and `main` returns 14 instead.
 * No output is written: `write` targets an invalid descriptor on purpose.
 *
 * ⓘ LINUX ONLY, deliberately: its syscall numbers and register conventions are
 * the kernel's documented, stable ABI (📄 `syscall(2)`, "Architecture calling
 * conventions"); Apple documents no stable syscall interface and Windows'
 * system-call numbers change between builds.
 */

volatile long dss_neg1 = -1;
volatile long dss_eight = 8;

#if defined(__x86_64__)

enum { NR_write = 1, NR_getpid = 39, NR_rt_sigprocmask = 14, NR_exit = 60 };

static long sys0(long n) {
    register long rax __asm__("rax") = n;
    __asm__ volatile("syscall" : "+r"(rax) : : "rcx", "r11", "memory");
    return rax;
}

static long sys3(long n, long a1, long a2, long a3) {
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a1;
    register long rsi __asm__("rsi") = a2;
    register long rdx __asm__("rdx") = a3;
    __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx)
                     : "rcx", "r11", "memory");
    return rax;
}

static long sys4(long n, long a1, long a2, long a3, long a4) {
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a1;
    register long rsi __asm__("rsi") = a2;
    register long rdx __asm__("rdx") = a3;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                     : "rcx", "r11", "memory");
    return rax;
}

#elif defined(__aarch64__)

enum { NR_write = 64, NR_getpid = 172, NR_rt_sigprocmask = 135, NR_exit = 93 };

static long sys0(long n) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

static long sys3(long n, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static long sys4(long n, long a1, long a2, long a3, long a4) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    register long x3 __asm__("x3") = a4;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
}

#else
#error "asm_syscall_wrapper_linux: no arm for this architecture"
#endif

int main(void) {
    long const pid = sys0(NR_getpid);
    if (pid <= 0 || sys0(NR_getpid) != pid) return 11;
    char byte = 'x';
    if (sys3(NR_write, dss_neg1, (long)&byte, 1) != -9) return 12;
    unsigned long old[2] = {0, 0};
    if (sys4(NR_rt_sigprocmask, 0, 0, (long)&old[0], dss_eight) != 0) return 13;
    sys3(NR_exit, 42, 0, 0);
    return 14;
}
