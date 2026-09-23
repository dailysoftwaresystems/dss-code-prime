/* AArch64 inline-assembly spellings GNU as and clang accept, which DSS refused.
 * D-ASM-DIALECT-GAPS-A-REFERENCE-ASSEMBLER-ACCEPTS.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. `add %w0, %w1, #2` on an `int`, `ldr %0,
 * [%1, #8]` through the pointer an operand holds, and `cbz`/`cbnz` to a C label
 * through `asm goto` are the everyday shapes of aarch64 inline assembly, and
 * each was REFUSED at the P68 round-7 base: the 32-bit immediate add had no
 * encoding ("no candidate target opcode encodes that shape"), a placeholder
 * inside `[...]` was a grammar error, and `cbz`/`cbnz` were unknown mnemonics.
 * ✔MEASURED 2026-09-21: aarch64-linux-gnu-gcc 13.3.0 and clang 18.1.3 run this
 * file to 42 at -O0 and -O2 under qemu-aarch64.
 *
 * Exit codes — one per template:
 *   11  `add %w0, %w1, #2` — the W-form immediate add
 *   12  `sub %w0, %w1, #4054` — the W-form immediate subtract
 *   13  `add %w0, %w1, #4096` — past imm12, the shifted form
 *   14  `ldr %0, [%1]` — a placeholder as the base
 *   15  `ldr %0, [%1, #8]` — a placeholder base with an offset
 *   16  `cbnz` on a non-zero value takes the C label
 *   17  `cbz` on a non-zero value falls through
 *   18  `cbz %w0` on a zero `int` takes the C label
 *   42  every check passed
 * ★ The `release` arm: every input comes from a `volatile`, so the optimized
 * pipeline keeps each template's operands and each branch real. */

static volatile int seed32 = 40;
static volatile long seed64 = 40;

int main(void) {
    int i = seed32, r;
    long l = seed64, rl;
    long cells[2];

    __asm__("add %w0, %w1, #2" : "=r"(r) : "r"(i));
    if (r != 42) return 11;

    __asm__("sub %w0, %w1, #4054" : "=r"(r) : "r"(i + 4056));
    if (r != 42) return 12;

    __asm__("add %w0, %w1, #4096" : "=r"(r) : "r"(i - 4094));
    if (r != 42) return 13;

    cells[0] = l + 2;
    cells[1] = l + 2;
    __asm__("ldr %0, [%1]" : "=r"(rl) : "r"(cells) : "memory");
    if (rl != 42) return 14;
    cells[0] = 0;
    __asm__("ldr %0, [%1, #8]" : "=r"(rl) : "r"(cells) : "memory");
    if (rl != 42) return 15;

    __asm__ goto("cbnz %0, %l[nonzero]" : : "r"(l) : : nonzero);
    return 16;
nonzero:
    __asm__ goto("cbz %0, %l[zero]" : : "r"(l) : : zero);
    goto int_zero;
zero:
    return 17;
int_zero:
    __asm__ goto("cbz %w0, %l[done]" : : "r"(i - 40) : : done);
    return 18;
done:
    return 42;
}
