// CU #1 of the cross-CU jump-table example.
//
// Deliberately holds NO switch of its own: the point is that the jump table lives in
// a DIFFERENT translation unit, so the whole-program merge has to move it and remap
// what it names. See dispatch.c for the mechanism.
//
// Exits 42 iff every arm of the table dispatched to the right block:
//   10+20+…+120 = 780, and the out-of-range arm returns -1.
//   780 - 738 = 42.
// A table slot bound to the wrong block, or to a stale address, changes the sum — so
// the exit code witnesses the RELOCATION VALUES and not merely that the link
// succeeded. That distinction is the whole reason this example runs rather than only
// building: the defect it pins produced a loud link failure, but the same wrong
// mapping applied one slot over would have been a silent wrong branch.

int dispatch(int sel);

int main(void) {
    int sum = 0;
    for (int i = 0; i < 12; ++i) sum += dispatch(i);
    // Out-of-range must reach `default`, not a table slot.
    if (dispatch(99) != -1) return 1;
    if (dispatch(-1) != -1) return 2;
    return sum - 738;
}
