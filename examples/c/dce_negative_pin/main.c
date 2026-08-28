// D-OPT1-DCE-NEGATIVE-PIN (step 13.6 cycle 1, 2026-06-03): a STORE
// that LOOKS dead at a syntactic glance but is GENUINELY LIVE on the
// taken branch. `a = -1` means `a > 0` is false at runtime; the
// `x = 7` store inside the if-body is unreachable; the live value of
// `x` at `return x` is `100` (from the first store).
//
// **The trap a buggy DCE would fall into**: spotting that the
// `if (a > 0)` branch is taken under SOME source-level conditions,
// then reasoning "x is reassigned in one branch and the other read
// is `return x`, so the first store IS dead." Wrong — the first
// store dominates the join; the second is the conditional one.
//
// Future OPT1 (13.6) DCE differential-verification: a DCE pass that
// incorrectly elides `x = 100` returns 0 or garbage (whatever rax
// happens to hold). Exit code 100 pins the live-store survival
// contract.

int main() {
    int a;
    int x;
    a = -1;
    x = 100;
    if (a > 0) {
        x = 7;
    }
    return x;
}
