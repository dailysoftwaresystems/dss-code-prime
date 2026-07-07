/* c61 (D-CSUBSET-CONTINUE-SKIPS-SWITCH, error[H0002]): a `continue` inside a `switch`
 * that is inside a loop must target the enclosing LOOP, not the switch (C 6.8.6.2 — a
 * switch is a `break` target but TRANSPARENT to `continue`). DSS rejected it with
 * error[H0002] "continue resolves to a switch; continue can only target a loop" because
 * isBranchTargetKind counts a switch as a branch target and the continue's depth-0
 * resolution landed on the innermost switch. SQLite hits this in sqlite3VXPrintf
 * (sqlite3.c:33119 / 33311): a format-char loop whose body switches on the conversion
 * type, with `continue` inside the switch to advance to the next format character.
 *
 * FIX: `continue` skips switch frames — the verifier drops switches from the target
 * list (C 6.8.6.2), and MIR skips switch frames (invalid continueBB) in the branchStack
 * walk — so it targets the innermost loop. `break` is unchanged (a switch IS a break
 * target).
 *
 * RED-ON-DISABLE: revert the fix -> error[H0002] (does not compile).
 *
 * VALUE-CORRECT + SENSITIVE: when i==2 the `case 2: continue;` must continue the FOR
 * loop, SKIPPING the post-switch `acc += 5`. If `continue` wrongly broke out of the
 * switch (ran the post-switch code), i==2 would add 5 and the total would be 47, not 42.
 *   i=0 default: +2 +5 = 7
 *   i=1 default: +2 +5 = 14
 *   i=2 case 2 : +14, continue (skips +5) = 28
 *   i=3 default: +2 +5 = 35
 *   i=4 default: +2 +5 = 42
 */
int run(void) {
    int acc = 0;
    int i;
    for (i = 0; i < 5; i = i + 1) {
        switch (i) {
            case 2:
                acc = acc + 14;
                continue;          /* targets the FOR loop, skipping `acc += 5` below */
            default:
                acc = acc + 2;
                break;
        }
        acc = acc + 5;             /* reached for every i except i==2 (which continues) */
    }
    return acc;                    /* 42 */
}

int main(void) {
    return run();
}
