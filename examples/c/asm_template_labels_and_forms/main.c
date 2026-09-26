/* LABELS INSIDE AN INLINE-ASM TEMPLATE, AND THE TEMPLATE TEXT FORMS gcc AND
 * clang EXPAND (P68 round 8 part 4 —
 * D-ASM-LABELS-INSIDE-A-TEMPLATE-AND-NUMERIC-LOCAL-LABELS-REFUSED and
 * D-ASM-TEMPLATE-FORMS-A-REFERENCE-EXPANDS-REFUSED).
 *
 * A label a template defines is a block of the STATEMENT's own body: nothing
 * the register allocator writes can land inside it. Every shape below changes
 * the exit code when it resolves wrongly:
 *   loop_numeric  — a counted loop on `1:` / `1b`                      (6)
 *   loop_twice    — `1:` defined TWICE in one template; each `1b` takes
 *                   the NEAREST one before it                          (9)
 *   loop_named    — a loop on a label made unique by `%=`, in TWO
 *                   statements of one function (each its own number)   (12)
 *   skip_forward  — `1f` over one instruction, both ways               (101, 8)
 *   cross_nine    — a `9f` written after one `9:`, which must reach the
 *                   NEXT `9:`, never the earlier one                   (11)
 *   forms         — x86: `mov{l} {%1, %0|%0, %1}` (the AT&T arm of the
 *                   dialect alternatives) and `%;%+%^%!` (gcc's punctuation
 *                   codes that print nothing) in front of a real
 *                   instruction; `%{ %} %| %* %= %~` inside a comment.
 *                   aarch64: `%{ %} %=` inside a comment.              (5)
 * gcc 13.3.0 and clang 18.1.3 run this to 42 on x86_64 and aarch64 (qemu).
 */
#if defined(__x86_64__)
static int loop_numeric(int n) {
    int acc = 0;
    __asm__("1:\n\t"
            "addl $2, %0\n\t"
            "subl $1, %1\n\t"
            "jnz 1b"
            : "+r"(acc), "+r"(n) : : "cc");
    return acc;
}
static int loop_twice(void) {
    int acc = 0;
    __asm__("1:\n\t"
            "addl $1, %0\n\t"
            "cmpl $5, %0\n\t"
            "jl 1b\n"
            "1:\n\t"
            "addl $2, %0\n\t"
            "cmpl $9, %0\n\t"
            "jl 1b"
            : "+r"(acc) : : "cc");
    return acc;
}
static int loop_named(int n) {
    int acc = 0, m = n;
    __asm__("again%=:\n\t"
            "addl $3, %0\n\t"
            "subl $1, %1\n\t"
            "jnz again%="
            : "+r"(acc), "+r"(n) : : "cc");
    __asm__("again%=:\n\t"
            "addl $1, %0\n\t"
            "subl $1, %1\n\t"
            "jnz again%="
            : "+r"(acc), "+r"(m) : : "cc");
    return acc;
}
static int skip_forward(int x) {
    __asm__("cmpl $0, %0\n\t"
            "jne 1f\n\t"
            "movl $100, %0\n"
            "1:\n\t"
            "addl $1, %0"
            : "+r"(x) : : "cc");
    return x;
}
static int cross_nine(void) {
    int acc = 0;
    __asm__("9:\n\t"
            "addl $1, %0\n\t"
            "cmpl $1, %0\n\t"
            "jne 8f\n\t"
            "jmp 9f\n"
            "8:\n\t"
            "addl $100, %0\n"
            "9:\n\t"
            "addl $10, %0"
            : "+r"(acc) : : "cc");
    return acc;
}
static int forms(int x) {
    int r;
    __asm__("mov{l} {%1, %0|%0, %1}" : "=r"(r) : "r"(x));
    __asm__("%;%+%^%!addl %1, %0 # %{ %} %| %* %= %~" : "+r"(r) : "r"(x) : "cc");
    return r;
}
#elif defined(__aarch64__)
static int loop_numeric(int n) {
    int acc = 0;
    __asm__("1:\n\t"
            "add %w0, %w0, #2\n\t"
            "sub %w1, %w1, #1\n\t"
            "cbnz %w1, 1b"
            : "+r"(acc), "+r"(n) : : "cc");
    return acc;
}
static int loop_twice(void) {
    int acc = 0;
    __asm__("1:\n\t"
            "add %w0, %w0, #1\n\t"
            "cmp %w0, #5\n\t"
            "b.lt 1b\n"
            "1:\n\t"
            "add %w0, %w0, #2\n\t"
            "cmp %w0, #9\n\t"
            "b.lt 1b"
            : "+r"(acc) : : "cc");
    return acc;
}
static int loop_named(int n) {
    int acc = 0, m = n;
    __asm__("again%=:\n\t"
            "add %w0, %w0, #3\n\t"
            "sub %w1, %w1, #1\n\t"
            "cbnz %w1, again%="
            : "+r"(acc), "+r"(n) : : "cc");
    __asm__("again%=:\n\t"
            "add %w0, %w0, #1\n\t"
            "sub %w1, %w1, #1\n\t"
            "cbnz %w1, again%="
            : "+r"(acc), "+r"(m) : : "cc");
    return acc;
}
static int skip_forward(int x) {
    __asm__("cbnz %w0, 1f\n\t"
            "mov %w0, #100\n"
            "1:\n\t"
            "add %w0, %w0, #1"
            : "+r"(x));
    return x;
}
static int cross_nine(void) {
    int acc = 0;
    __asm__("9:\n\t"
            "add %w0, %w0, #1\n\t"
            "cmp %w0, #1\n\t"
            "b.ne 8f\n\t"
            "b 9f\n"
            "8:\n\t"
            "add %w0, %w0, #100\n"
            "9:\n\t"
            "add %w0, %w0, #10"
            : "+r"(acc) : : "cc");
    return acc;
}
static int forms(int x) {
    int r = x;
    __asm__("add %w0, %w0, %w1 // %{ %} %=" : "+r"(r) : "r"(x));
    return r - x + x;
}
#else
#error "this example's templates are x86_64 and aarch64 assembly"
#endif

int main(void) {
    if (loop_numeric(3) != 6) return 11;
    if (loop_twice() != 9) return 12;
    if (loop_named(3) != 12) return 13;
    if (skip_forward(0) != 101) return 14;
    if (skip_forward(7) != 8) return 15;
    if (cross_nine() != 11) return 16;
    if (forms(5) != 10) return 17;
    return 42;
}
