// D-OPT1-CSE-NONCOMMUTATIVE-PIN (step 13.6 cycle 1, 2026-06-03):
// negative pin for CSE / value-numbering. `a - b` and `b - a` are
// DIFFERENT expressions despite sharing operands; a CSE that
// incorrectly treats them as equivalent (e.g. via a hash that's
// order-insensitive on commutative-looking-but-non-commutative ops)
// produces wrong code.
//
// With a=5, b=3: x = (a-b)*2 = 4; y = (b-a)*3 = -6. return x + y +
// 60 = 4 - 6 + 60 = 58.
//
// **The trap**: a CSE that merges `(a - b)` and `(b - a)` into a
// single value (returning `(a - b) = 2` for both reads) produces
// y = 2 * 3 = 6 → return 4 + 6 + 60 = 70. The 12-point exit-code
// distance (58 correct vs 70 buggy-CSE) is bisectable.
//
// Companion to `cse_candidate/` (positive CSE pin where the shared
// sub-tree IS legitimately mergeable): together they pin the CSE's
// equivalence relation respects operator commutativity (signed
// subtraction is NOT commutative; addition IS but isn't tested here).

int main() {
    int a;
    int b;
    int x;
    int y;
    a = 5;
    b = 3;
    x = (a - b) * 2;
    y = (b - a) * 3;
    return x + y + 60;
}
