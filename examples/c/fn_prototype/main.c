// D-CSUBSET-FN-PROTOTYPE witness: a bare function PROTOTYPE (`int f(int);`) is a
// function DECLARATION that MERGES with a later definition (the definition
// provides the body). This enables FORWARD / MUTUAL recursion: `is_even` calls
// `is_odd` before `is_odd` is defined, resolved through the forward prototype.
//
//   int is_odd(int n);                 <- prototype (no body)
//   int is_even(int n) { ... is_odd }  <- definition; forward-calls is_odd
//   int is_odd(int n)  { ... is_even } <- definition; MERGES with the prototype
//
// Each result is value-divergent, so a merge bug (a spurious FnSig-typed data
// global for the prototype, or the forward call failing to resolve) yields
// wrong values -> exit 7, not 42:
//   is_even(10) = 1   is_odd(7) = 1   is_even(4) = 1
// Correct merge => (1 && 1 && 1) => 42.
//
// RED-ON-DISABLE: revert the semantic merge (restore the Pass-1.5
// S_InvalidFunctionDeclarator rejection of `int is_odd(int n);`) -> the program
// fails to COMPILE (the prototype is rejected), so this example no longer
// builds at all -> the runner reports a compile failure instead of exit 42.
//
// Runs on x86_64 (PE+ELF) and arm64 (ELF qemu, Mach-O macos leg): the merge is
// a front-end (semantic + HIR) feature, target-agnostic — the emitted code is
// two ordinary mutually-recursive functions.

int is_odd(int n);                 // prototype — declaration without definition

int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }
int is_odd(int n)  { return n == 0 ? 0 : is_even(n - 1); }

int main(void) {
    int a = is_even(10);   // 1
    int b = is_odd(7);     // 1
    int c = is_even(4);    // 1
    return (a == 1 && b == 1 && c == 1) ? 42 : 7;
}
