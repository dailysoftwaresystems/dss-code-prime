// D-OPT1-CONST-FOLD-MIR-PIN (step 13.6 cycle 1, 2026-06-03): a
// constant-foldable sub-tree (`5 + 3`) embedded in an expression
// whose other operand reaches from a global (live edge). The
// canonical OPT1 const-fold rewrite folds `5 + 3 → 8`, leaving
// `return 8 * 2 + a` (= 16 + 26 = 42). A correct const-fold must
// preserve the live read of the global `a`; a buggy fold that
// over-constants (e.g. folding the entire expression to a literal
// without honoring the global load) shifts the exit code.
//
// **The trap**: a fold that incorrectly cascades into the surrounding
// `x * 2 + a` expression — say, dropping the `+ a` term as
// "trivially folded" — returns 16 instead of 42.
//
// Also pins the foldable-sub-tree-with-live-edge shape that
// `examples/c-subset/cse_candidate/` shows from the CSE angle: the
// shared sub-tree pattern in both pins gives OPT1 distinct views
// of the same value-numbering substrate.

int a = 26;
int main() {
    int x;
    x = 5 + 3;
    return x * 2 + a;
}
