/* C23's `unreachable()` from <stddef.h> (7.21.1), and the GNU `__builtin_unreachable()` it expands to
 * (P68 round 12, D-C-STDDEF-H-LACKS-UNREACHABLE, fold S1b). Before S1b both were undeclared (S_UndeclaredIdentifier).
 *
 * Each use sits only where control cannot arrive — the tail after a switch over every enumerator, a switch default
 * the callers exclude, the tail after a branch that returned, an expression arm the operand never takes — so the trap
 * lowering (MIR's Unreachable terminator: `ud2` / `brk #0`) and the program's exit code coexist. The no-fall-through
 * property itself is pinned by tests/mir/test_builtin_unreachable_lowering.cpp; this example pins that both
 * spellings are ACCEPTED where gcc and clang accept them — a non-void function whose last path ends in one is
 * complete — and that the program around them computes the right value. */
#include <stddef.h>

enum colors { red, green, blue };

/* C23 7.21.1 EXAMPLE 2's own shape: a switch over every enumerator, then unreachable() in the tail. */
static int channel(enum colors c) {
    switch (c) {
        case red: return 0;
        case green: return 1;
        case blue: return 2;
    }
    unreachable();
}

static int pick(int x) {
    switch (x) {
        case 3: return 7;
        case 5: return 11;
        default: unreachable();   /* the callers pass only 3 and 5 */
    }
}

static int twice(int x) {
    if (x >= 0) return x * 2;
    __builtin_unreachable();      /* the builtin itself, spelled directly */
}

static int sign(int x) { return x > 0 ? 1 : (__builtin_unreachable(), 0); }   /* expression position */

volatile int dss_three = 3;

int main(void) {
    int const a = pick(dss_three);                          /* 7 */
    int const b = pick(dss_three + 2);                      /* 11 */
    int const c = twice(dss_three * 4);                     /* 24 */
    int const d = channel((enum colors)(dss_three - 1));    /* blue: 2 */
    return a + b + c + d + sign(dss_three) - 3;             /* 42 */
}
