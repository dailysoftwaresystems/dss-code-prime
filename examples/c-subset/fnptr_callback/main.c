// FC4 c2 — fold-resistant indirect-call witness (plan-lock MUST-FIX 7:
// fold-resistance BY CONSTRUCTION, not by hoping a pass list stays
// weak).
//
//   * `apply` is SELF-RECURSIVE -> the SCC pass refuses to inline it
//     (a self-loop is its own SCC), so `f` can never be replaced by a
//     constant via inlining;
//   * `f` arrives as a CALL-BOUNDARY argument and is passed onward as
//     one -> a function-pointer arg ESCAPES the inliner's
//     non-GlobalAddr-callee refusal too;
//   * so `f` stays an Arg SSA value in EVERY pipeline (baseline AND
//     full-release-like), and `(*f)(v)` stays a genuine indirect call
//     through a register;
//   * `(*f)(v)` (not `f(v)`) additionally runtime-witnesses the
//     C 6.5.3.2p4 deref-decay fold: `*` on a function pointer is the
//     identity for call purposes — the HIR fold must NOT emit a memory
//     load through the code pointer (which would execute garbage).
//
// Exit arithmetic: apply(&inc, 2, 41) recurses n=2 -> 1 -> 0, then
// calls inc(41) = 42. A clobbered/garbage callee crashes or returns
// non-42; delta from the recursion-miscounts is nonzero mod 256.
int inc(int v) { return v + 1; }
int apply(int (*f)(int), int n, int v) {
    if (n) { return apply(f, n - 1, v); }
    return (*f)(v);
}
int main() { return apply(&inc, 2, 41); }
