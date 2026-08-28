// D-OPT1-COPY-PROP-JOIN-PIN (step 13.6 cycle 1, 2026-06-03):
// copy-prop across a CFG join. `b = a` inside the taken arm of an
// if/else; `a` is REASSIGNED after the join. A buggy copy-prop that
// replaces `return b` with `return a` (treating `b = a` as a
// forwarding edge without honoring the phi at the join) reads the
// post-join value of `a` (= 999) instead of the pre-join captured
// value (= 10).
//
// **The trap**: copy-prop assumes `b = a; ...; use(b)` can be
// rewritten as `use(a)` whenever no intervening def of `a` exists
// on EVERY path from `b = a` to `use(b)`. With cond=1 here, the
// `else` arm is unreached BUT THE COMPILER DOESN'T KNOW THAT at
// optimize time. The phi at the join collapses to `b = a` only when
// the `else` arm sets b too; in either case, the post-join store
// `a = 999` is on a path from any pre-join store of `a` to `return
// b`. The phi-aware copy-prop respects this; the naive one doesn't.
//
// Pre-fix expected exit code 10 (b captured a at the moment of the
// `b = a` store). A buggy copy-prop returns 999.

int main() {
    int cond;
    int a;
    int b;
    cond = 1;
    a = 10;
    if (cond > 0) {
        b = a;
    } else {
        b = 50;
    }
    a = 999;
    return b;
}
