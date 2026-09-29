/* x86-64 inline-assembly spellings GNU as and clang accept, which DSS refused.
 * D-ASM-DIALECT-GAPS-A-REFERENCE-ASSEMBLER-ACCEPTS.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Every template below is one a real program
 * writes — an unsuffixed `mov`, a `lea` through the pointer the operand holds,
 * `test` + `sete` for a boolean, a double's bits moved to a general register,
 * a scratch xmm register named in the template — and every one of them was
 * REFUSED at the P68 round-7 base (`unknown mnemonic`, a grammar error at the
 * placeholder inside the memory operand, or "register '%1' is 64 bits wide but
 * register 'xmm5' is 128 bits"). ✔MEASURED 2026-09-21: gcc 13.3.0 and clang
 * 18.1.3 run this file to 42 at -O0 and -O2 on x86_64 Linux, and mingw-w64 gcc
 * 13.2.0 on Windows.
 *
 * Exit codes — one per template, so a wrong byte names its own line:
 *   11  unsuffixed `mov` loads through a pointer operand (`mov (%1), %0`)
 *   12  unsuffixed `add` of two registers
 *   13  `leal 2(%1), %0` — a 32-bit address computation
 *   14  `lea (%1,%2,8), %0` — base, index and scale all placeholders
 *   15  `testl %1, %1` + `sete %b0` on a zero value
 *   16  the same on a non-zero value
 *   17  `movq %1, %0`: a double's bits into a general register
 *   18  `movd %1, %0`: an int's bits into a float register
 *   19  a scratch `%xmm5` named in the template beside a `"x"` operand
 *   20  `movapd` between two xmm operands
 *   21  a byte loaded through base+index placeholders into a `char`
 *   42  every check passed
 * ★ The `release` arm: every input comes from a `volatile`, so the optimized
 * pipeline keeps each template's operands real. */

static volatile long long seed64 = 40;
static volatile int seed32 = 2;
static volatile double seedd = 21.0;

int main(void) {
    long long v = seed64, r64;
    int i = seed32, r32;
    double d = seedd, rd;
    float f;
    unsigned char r8 = 0;
    long long cells[4] = {0, 0, 0, 0};
    unsigned char bytes[4] = {7, 8, 42, 9};
    unsigned char *bp = bytes;
    long long idx = 2;

    cells[2] = v + 2;
    {
        long long *cp = &cells[2];
        __asm__("mov (%1), %0" : "=r"(r64) : "r"(cp) : "memory");
        if (r64 != 42) return 11;
    }
    r64 = v;
    __asm__("add %1, %0" : "+r"(r64) : "r"((long long)i));
    if (r64 != 42) return 12;

    __asm__("leal 2(%1), %0" : "=r"(r32) : "r"(v));
    if (r32 != 42) return 13;

    {
        long long *base = cells;
        __asm__("lea (%1,%2,8), %0" : "=r"(r64) : "r"(base), "r"(idx));
        if (r64 != (long long)&cells[2]) return 14;
    }

    r8 = 0;
    {
        int zero = i - 2;
        __asm__("testl %1, %1\n\tsete %0" : "=q"(r8) : "r"(zero) : "cc");
        if (r8 != 1) return 15;
        __asm__("testl %1, %1\n\tsete %0" : "=q"(r8) : "r"(i) : "cc");
        if (r8 != 0) return 16;
    }

    __asm__("movq %1, %0" : "=r"(r64) : "x"(d));
    if (r64 != 0x4035000000000000LL) return 17;

    {
        int bits = 0x42280000 + (i - 2);   /* 42.0f */
        __asm__("movd %1, %0" : "=x"(f) : "r"(bits));
        if (f != 42.0f) return 18;
    }

    __asm__("movsd %1, %%xmm5\n\taddsd %%xmm5, %%xmm5\n\tmovsd %%xmm5, %0"
            : "=x"(rd) : "x"(d) : "xmm5");
    if (rd != 42.0) return 19;

    __asm__("movapd %1, %0" : "=x"(rd) : "x"(d));
    if (rd != 21.0) return 20;

    __asm__("mov (%1,%2), %0" : "+q"(r8) : "r"(bp), "r"(idx) : "memory");
    if (r8 != 42) return 21;

    return 42;
}
