/* ONE ELEMENT OF A VECTOR REGISTER IN AN aarch64 INLINE-ASM TEMPLATE —
 * `%1.d[1]`, `%1.s[3]`, `%0.b[0]` (D-ASM-DIALECT-GAPS-A-REFERENCE-ASSEMBLER-ACCEPTS).
 *
 * GCC spells one lane of a SIMD operand as the operand followed by a size and
 * an index, and the reference compilers print the operand as its `v` register:
 * `umov %0, %1.d[1]` reads the high 64 bits of a 128-bit value. The P68
 * round-7 base refused the template at its grammar. A `long double` is
 * IEEE binary128 on aarch64 Linux and travels in ONE `"w"` register, so its
 * halves and words are elements a template can name; each check compares what
 * the element instruction produced with the value's own bytes. The last check
 * writes ONE byte lane of a `"+w"` operand and requires the other fifteen
 * unchanged — `ins` keeps the lanes it does not write, so the operand's old
 * value has to reach the instruction.
 * Exit 11-14 names the check that failed.
 */
#include <string.h>

static int read_halves(void) {
    long double x = 1.5L;
    unsigned long want[2];
    unsigned long hi, lo;
    memcpy(want, &x, sizeof want);
    __asm__("umov %0, %1.d[1]" : "=r"(hi) : "w"(x));
    __asm__("mov %0, %1.d[0]" : "=r"(lo) : "w"(x));
    return hi == want[1] && lo == want[0];
}

static int read_a_word(void) {
    long double x = -2.25L;
    unsigned int want[4];
    unsigned int w3;
    memcpy(want, &x, sizeof want);
    __asm__("mov %w0, %1.s[3]" : "=r"(w3) : "w"(x));
    return w3 == want[3];
}

static int write_halves(void) {
    long double x = 0.0L;
    long double const want = 1.5L;
    unsigned long parts[2];
    memcpy(parts, &want, sizeof parts);
    __asm__("ins %0.d[0], %1\n\tmov %0.d[1], %2"
            : "+w"(x)
            : "r"(parts[0]), "r"(parts[1]));
    return x == want;
}

static int write_one_byte(void) {
    long double x = 1.5L;
    unsigned char before[16];
    unsigned char after[16];
    unsigned int v = 0x5a;
    memcpy(before, &x, sizeof before);
    __asm__("ins %0.b[0], %w1" : "+w"(x) : "r"(v));
    memcpy(after, &x, sizeof after);
    if (after[0] != 0x5a) return 0;
    for (int i = 1; i < 16; ++i) {
        if (after[i] != before[i]) return 0;
    }
    return 1;
}

int main(void) {
    if (!read_halves()) return 11;
    if (!read_a_word()) return 12;
    if (!write_halves()) return 13;
    if (!write_one_byte()) return 14;
    return 42;
}
