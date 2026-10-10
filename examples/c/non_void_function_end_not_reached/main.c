/* THE CLOSING BRACE OF A NON-VOID FUNCTION IS NOT REACHED — AND NOTHING SAYS IT IS
 * (D-C-A-NON-VOID-FUNCTION-WHOSE-END-IS-REACHABLE-IS-REFUSED, the other direction).
 *
 * `examples/c/non_void_function_end_reached` holds the functions whose end IS
 * reached: compiled, reported once each, returned from. This file holds the ones
 * whose end no execution reaches although their last statement is not a `return`
 * — the compiler has to SEE that, from a constant's value or from a callee that
 * does not return, or it reports an end nothing arrives at. The manifest forbids
 * that report (`forbidDiagnostics`), and every function here RUNS.
 *
 * "Control never continues past this statement" is also a CLAIM the back end may
 * act on, so the second half of the file is the statements that must NOT be
 * claimed: each of them continues, and the code after it runs and is checked.
 *
 * `leave` and its siblings REALLY do not return: they leave through `longjmp`, so
 * each never-continuing statement is executed, not merely compiled, and `run`
 * reports where control went (a function's value, or minus the code it left
 * with). Every argument comes from `argc`, so no check folds at compile time.
 */
#include <setjmp.h>
#include <stddef.h>

static jmp_buf back;
static volatile int left_with;
static volatile int steps;

_Noreturn void leave(int code);
void leave(int code) { left_with = code; longjmp(back, 1); }

static void stay(int count) { steps += count; }

/* ── the end is not reached: a constant condition, a `do` body that never reaches
 *    its condition, an expression that cannot finish ────────────────────────── */
int do_once(int x) { do { return x + 1; } while (0); }
int if_one(int x) { if (1) return x + 2; }
int if_zero_else(int x) { if (0) x = 100; else return x + 3; }
int if_enumerator(int x) { enum { ON = 1 }; if (ON) return x + 4; }
int both_arms_leave(int x) { x ? leave(11) : leave(12); }
int comma_leaves(int x) { (void)x, leave(13); }
int cast_leaves(int x) { (void)x; (void)leave(14); }

/* ── a condition that is a constant only through the target's LAYOUT, or through
 *    an answer the compiler computed when it typed the expression ───────────── */
struct P { char a; int b; };
int by_sizeof(int x) { if (sizeof(int) >= 1) return x + 5; }
int by_sizeof_value(int x) { if (sizeof x >= 1) return x + 6; }
int by_alignof(int x) { if (_Alignof(double) >= 1) return x + 7; }
int by_offsetof(int x) { if (offsetof(struct P, b) >= 1) return x + 8; }
int by_member_address(int x) { if ((size_t)&((struct P *)0)->b >= 1) return x + 9; }
int by_generic(int x) { if (_Generic(x, int: 1, default: 0)) return x + 10; }
int while_sizeof(int x) { while (sizeof(int)) { if (x) return x + 11; x = 1; } }
int for_sizeof(int x) { for (; sizeof(int); ) { if (x) return x + 12; x = 1; } }

/* ── a call that PRECEDES the declaration which adds `_Noreturn`: the function
 *    does not return whichever declaration the call could see ───────────────── */
void late(int code);
int late_plain(int x) { (void)x; late(21); }
int late_both_arms(int x) { x ? late(22) : late(23); }
int late_comma(int x) { (void)x, late(24); }
int late_cast(int x) { (void)x; (void)late(25); }
_Noreturn void late(int code);
void late(int code) { left_with = code; longjmp(back, 1); }

/* ── `_Noreturn` on ONE declaration among several ───────────────────────────── */
_Noreturn void first_only(int code);
void first_only(int code);
void later_only(int code);
_Noreturn void later_only(int code);
int via_first_only(int x) { x ? first_only(31) : first_only(32); }
int via_later_only(int x) { x ? later_only(33) : later_only(34); }
void first_only(int code) { left_with = code; longjmp(back, 1); }
void later_only(int code) { left_with = code; longjmp(back, 1); }

/* ── MUST NOT BE CLAIMED (i): a `case` / `default` label of the ENCLOSING switch
 *    inside the arm a constant `if` never takes. The switch jumps into that arm,
 *    it runs to its end, and the statement after the `if` is then executed. ──── */
int label_in_dead_then(int x) {
    switch (x) {
    case 0:
        if (0) { case 1: x += 3; } else return 50;
        x += 1;
        break;
    default:
        return 60;
    }
    return x;
}
int label_in_dead_else(int x) {
    switch (x) {
    case 0:
        if (1) return 51; else { case 1: x += 5; }
        x += 1;
        break;
    default:
        return 61;
    }
    return x;
}
int default_in_dead_then(int x) {
    switch (x) {
    case 0:
        if (0) { default: x += 7; } else return 52;
        x += 1;
        break;
    }
    return x;
}

/* ── (ii) the only way out is a `goto` to a label LATER in the function: the
 *    statement never completes, and the label's code runs ───────────────────── */
int do_leaves_by_goto(int x) { do { if (x) goto out; return 1; } while (0); return 2; out: return x + 3; }
int if_leaves_by_goto(int x) { if (1) goto out; return 2; out: return x + 4; }

/* ── (iii) a callee that is not a direct reference to a function that does not
 *    return: when the live function is chosen the call returns and control goes on */
int conditional_callee(int x) { (x ? leave : stay)(15); return 5; }
int pointer_callee(int x) { void (*p)(int) = x ? leave : stay; p(16); return 6; }
int one_arm_returns(int x) { x ? leave(17) : stay(18); return 7; }

/* ── NOT A CONSTANT: `sizeof` of a variably modified operand is evaluated when the
 *    program runs, so both outcomes of each branch happen ───────────────────── */
#ifndef __STDC_NO_VLA__
int vla_type(int n) { if (sizeof(int[n]) > 8) return 1; return 2; }
int vla_object(int n) { int v[n]; v[0] = n; if (sizeof v > 8) return 1; return 2 + v[0] - n; }
int vla_loop(int n) { while (sizeof(int[n]) > 8) { n--; } return n; }
#endif

/* ── the same evaluator's third consumer: an index designator ───────────────── */
struct Q { char a; char c; short b; };
static int table[8] = { [sizeof(char)] = 5, [sizeof(int)] = 7,
                        [offsetof(struct Q, b)] = 3, [_Alignof(short) + 4] = 9 };

/* f(arg)'s value, or minus the code it left with. */
static int run(int (*f)(int), int arg) {
    if (setjmp(back) != 0) return -left_with;
    return f(arg);
}

int main(int argc, char **argv) {
    int const zero = argc - 1;
    int const one = argc;
    int local[8] = { [sizeof(short)] = 6, [_Alignof(int)] = 8 };
    (void)argv;

    if (run(do_once, one) != 2) return 1;
    if (run(if_one, one) != 3) return 2;
    if (run(if_zero_else, one) != 4) return 3;
    if (run(if_enumerator, one) != 5) return 4;
    if (run(both_arms_leave, one) != -11) return 5;
    if (run(both_arms_leave, zero) != -12) return 6;
    if (run(comma_leaves, one) != -13) return 7;
    if (run(cast_leaves, one) != -14) return 8;

    if (run(by_sizeof, one) != 6) return 9;
    if (run(by_sizeof_value, one) != 7) return 10;
    if (run(by_alignof, one) != 8) return 11;
    if (run(by_offsetof, one) != 9) return 12;
    if (run(by_member_address, one) != 10) return 13;
    if (run(by_generic, one) != 11) return 14;
    if (run(while_sizeof, zero) != 12) return 15;
    if (run(for_sizeof, zero) != 13) return 16;

    if (run(late_plain, one) != -21) return 17;
    if (run(late_both_arms, one) != -22) return 18;
    if (run(late_both_arms, zero) != -23) return 19;
    if (run(late_comma, one) != -24) return 20;
    if (run(late_cast, one) != -25) return 21;
    if (run(via_first_only, one) != -31) return 22;
    if (run(via_first_only, zero) != -32) return 23;
    if (run(via_later_only, one) != -33) return 24;
    if (run(via_later_only, zero) != -34) return 25;

    if (run(label_in_dead_then, zero) != 50) return 26;
    if (run(label_in_dead_then, one) != 5) return 27;
    if (run(label_in_dead_then, one + 1) != 60) return 28;
    if (run(label_in_dead_else, zero) != 51) return 29;
    if (run(label_in_dead_else, one) != 7) return 30;
    if (run(label_in_dead_else, one + 1) != 61) return 31;
    if (run(default_in_dead_then, zero) != 52) return 32;
    if (run(default_in_dead_then, one + 4) != 13) return 33;

    if (run(do_leaves_by_goto, zero) != 1) return 34;
    if (run(do_leaves_by_goto, one) != 4) return 35;
    if (run(if_leaves_by_goto, one) != 5) return 36;

    if (run(conditional_callee, zero) != 5 || steps != 15) return 37;
    if (run(conditional_callee, one) != -15) return 38;
    if (run(pointer_callee, zero) != 6 || steps != 31) return 39;
    if (run(pointer_callee, one) != -16) return 40;
    if (run(one_arm_returns, zero) != 7 || steps != 49) return 41;
    if (run(one_arm_returns, one) != -17) return 43;

#ifndef __STDC_NO_VLA__
    if (run(vla_type, one) != 2) return 44;
    if (run(vla_type, one + 3) != 1) return 45;
    if (run(vla_object, one) != 2) return 46;
    if (run(vla_object, one + 3) != 1) return 47;
    if (run(vla_loop, one + 4) != 2) return 48;
#endif

    if (!(table[1] == 5 && table[4] == 7 && table[2] == 3 && table[6] == 9 && table[0] == 0)) return 49;
    if (!(local[2] == 6 && local[4] == 8 && local[0] == 0)) return 50;
    return 42;
}
