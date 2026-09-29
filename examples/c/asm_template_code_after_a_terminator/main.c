/* CODE AFTER A TERMINATOR INSIDE AN INLINE-ASM TEMPLATE, WITH NO LABEL BETWEEN
 * THEM (P68 round 8 — D-ASM-INSTRUCTION-AFTER-A-TERMINATOR-REFUSED).
 *
 * The assembler emits every template line where it stands; nothing branches to
 * a line that follows an unconditional branch without a label, so it never
 * runs. Each dead line changes the result if it does:
 *   skip(40)  — `add 100` after the jump to `1:`, and `add 1000` after the
 *               jump to `2:` at the template's end                   (42)
 *   pick(x)   — a conditional branch whose fall-through is the next line,
 *               then a dead line after the jump over the other arm   (42, 7)
 * gcc 13.3.0 and clang 18.1.3 run this to 42 on x86_64 and aarch64 (qemu), at
 * -O0 and -O2.
 */
#if defined(__x86_64__)
static int skip(int x) {
    __asm__("jmp 1f\n\t"
            "addl $100, %0\n"
            "1:\n\t"
            "addl $2, %0\n\t"
            "jmp 2f\n\t"
            "addl $1000, %0\n"
            "2:"
            : "+r"(x) : : "cc");
    return x;
}
static int pick(int x) {
    __asm__("cmpl $0, %0\n\t"
            "jne 1f\n\t"
            "movl $7, %0\n\t"
            "jmp 2f\n\t"
            "movl $500, %0\n"
            "1:\n\t"
            "movl $42, %0\n"
            "2:"
            : "+r"(x) : : "cc");
    return x;
}
#elif defined(__aarch64__)
static int skip(int x) {
    __asm__("b 1f\n\t"
            "add %w0, %w0, #100\n"
            "1:\n\t"
            "add %w0, %w0, #2\n\t"
            "b 2f\n\t"
            "add %w0, %w0, #1000\n"
            "2:"
            : "+r"(x));
    return x;
}
static int pick(int x) {
    __asm__("cmp %w0, #0\n\t"
            "b.ne 1f\n\t"
            "mov %w0, #7\n\t"
            "b 2f\n\t"
            "mov %w0, #500\n"
            "1:\n\t"
            "mov %w0, #42\n"
            "2:"
            : "+r"(x) : : "cc");
    return x;
}
#else
#error "this example's templates are x86_64 and aarch64 assembly"
#endif

int main(void) {
    if (skip(40) != 42) return 11;
    if (pick(0) != 7) return 12;
    return pick(5);
}
