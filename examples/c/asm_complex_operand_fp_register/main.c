/* D-MIR-ASM-BY-ADDRESS-OPERAND-BOUND-TO-A-REGISTER-RECEIVES-ITS-ADDRESS — the
 * `_Complex` values an FP-class asm register carries, the shapes CLANG carries
 * and gcc refuses (so this file is the clang-witnessed half of the corpus; the
 * gcc-witnessed half is `asm_value_operand_carriage_{arm64,x86_64}`).
 *
 * A `_Complex` lives in MEMORY in this compiler's middle end; bound to an FP
 * register constraint it must reach the template as its VALUE and leave it as
 * one. clang 18.1.3 runs this file to 42 at -O0 and -O2, natively on x86_64
 * and under qemu-aarch64; gcc 13.3.0 refuses every shape here ("inconsistent
 * operand constraints" on x86_64 "x"; aarch64 "=w" exhausts its reloads).
 *   x86_64: a `_Complex float` in one XMM register, each direction, and a
 *           `_Complex double` WRITTEN into one XMM register (all 16 bytes);
 *   aarch64: a `_Complex double` WRITTEN into one Q register.
 * Every check is BITWISE over the value's own bytes; each failing check has
 * its own exit code.
 */
#include <string.h>

typedef unsigned long long u64;
static const u64 W0 = 0x0123456789ABCDEFull, W1 = 0x0FEDCBA987654321ull;

#if defined(__x86_64__)
__attribute__((noinline)) static int cf_in(_Complex float z) {
    double d; u64 got, want;
    __asm__("movsd %1, %0" : "=x"(d) : "x"(z));
    memcpy(&got, &d, 8); memcpy(&want, &z, 8);
    return got == want;
}
__attribute__((noinline)) static int cf_out(double d) {
    _Complex float z; u64 got, want;
    __asm__("movsd %1, %0" : "=x"(z) : "x"(d));
    memcpy(&got, &z, 8); memcpy(&want, &d, 8);
    return got == want;
}
__attribute__((noinline)) static int cf_inout(_Complex float z, double d) {
    u64 got, want;
    __asm__("movsd %1, %0" : "+x"(z) : "x"(d));
    memcpy(&got, &z, 8); memcpy(&want, &d, 8);
    return got == want;
}
__attribute__((noinline)) static void cd_write(_Complex double *p, __int128 w) {
    _Complex double z;
    __asm__("movaps %1, %0" : "=x"(z) : "x"(w));
    *p = z;
}
__attribute__((noinline)) static int cd_out(void) {
    __int128 w; u64 in[2] = { W0, W1 }, got[2];
    memcpy(&w, in, 16);
    _Complex double z;
    cd_write(&z, w);
    memcpy(got, &z, 16);
    return got[0] == W0 && got[1] == W1;
}
#elif defined(__aarch64__)
__attribute__((noinline)) static void cd_write(_Complex double *p, u64 x) {
    _Complex double z;
    __asm__("fmov %d0, %1" : "=w"(z) : "r"(x));
    *p = z;
}
__attribute__((noinline)) static int cd_out(void) {
    _Complex double z; u64 got[2];
    cd_write(&z, W0);
    memcpy(got, &z, 16);
    return got[0] == W0 && got[1] == 0;   /* FMOV Dd, Xn zeroes the upper half */
}
#endif

int main(void) {
#if defined(__x86_64__)
    {
        _Complex float z; u64 p = W0; memcpy(&z, &p, 8);
        if (!cf_in(z)) return 11;
    }
    {
        double d; u64 p = W1; memcpy(&d, &p, 8);
        if (!cf_out(d)) return 12;
    }
    {
        _Complex float z; u64 p = W0; memcpy(&z, &p, 8);
        double d; u64 q = W1; memcpy(&d, &q, 8);
        if (!cf_inout(z, d)) return 13;
    }
#endif
    if (!cd_out()) return 14;
    return 42;
}
