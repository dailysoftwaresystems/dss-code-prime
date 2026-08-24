// c18 (positional macro expansion, C 6.10.3) -- the runtime witness. Proves the
// preprocessor does NOT retroactively apply a macro to uses that appear BEFORE
// its define, end to end through every target leg.
//
// This is SQLite's omit-feature pattern: a name is DECLARED (here, a global int),
// then later defined to a constant via a macro. The declaration must survive as
// an identifier -- if the macro clobbered it retroactively, "int base_val;" would
// become "int 40;" (a parse error) and no binary would be produced. The later
// uses (in the call arguments, AFTER the macro definitions) DO expand.
//
//   live order -> base_val = 40, margin_val = 2; add(40, 2) = 42 -> exit 42
//
// Fold-resistance (mirrors the FC14/c17 witnesses): base_val and margin_val reach
// add() as FUNCTION ARGUMENTS, so the baseline (unoptimized) arm keeps a live
// runtime add -- the result is not const-folded to a single immediate at exit.
// The optimizedPipelines release arm runs the shipped pipeline over the SAME
// source. A regressed (retroactive) expansion fails the BUILD (parse error), so a
// green exit 42 is the proof of positional semantics.

// base_val is DECLARED here, then defined as a macro on the next line. The
// declaration must survive (positional: the macro affects only later uses).
int base_val;
#define base_val 40

// margin_val: same pattern.
int margin_val;
#define margin_val 2

int add(int a, int b) {
    return a + b;
}

int main(void) {
    return add(base_val, margin_val);   // add(40, 2) = 42
}
