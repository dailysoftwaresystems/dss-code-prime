// D-CSUBSET-VLA C2: `sizeof` applied to a function-scope variable-length array is a
// RUNTIME value — the byte size frozen ONCE at the array's declaration (C 6.7.6.2p2),
// NOT a compile-time constant (a VLA's size is not a constant expression, C 6.6). This
// exercises the canonical element-count idiom `sizeof arr / sizeof arr[0]`, which for a
// VLA is (runtime Load of the decl-frozen byte size) / (static element size):
//
//   - `sizeof a`     lowers to a Load of a's hidden decl-frozen size slot (n*4 bytes).
//   - `sizeof a[0]`  is the ELEMENT (int) — a plain compile-time 4 (NOT a VLA sizeof).
//   - their ratio    == n == 42.
//
// `volatile` defeats constant-folding so n is genuinely runtime. main is a LEAF (no
// calls) — the C1b VLA frame-model scope. Two further strict checks ride the exit code:
// the ABSOLUTE byte size (`sizeof a == n*sizeof(int)`), and freeze-at-decl (mutating n
// AFTER the decl must not change `sizeof a`). Any of these breaking flips the exit code.
int main(void) {
    volatile int seed = 42;      // runtime length; volatile => no constant fold
    int n = seed;
    int a[n];                    // VLA: sizeof a == n*sizeof(int), frozen HERE
    int count = (int)(sizeof a / sizeof a[0]);   // runtime Load / static 4 == n == 42

    // Absolute byte size: sizeof a must equal n*sizeof(int) exactly (== 42*4 == 168).
    if (sizeof a != (unsigned long)count * sizeof(int)) {
        return 1;                // strict in-program pin: wrong bytes => harness RED
    }
    // Freeze-at-decl (C 6.7.6.2p2): the size was captured at the decl; mutating n now
    // must NOT change sizeof a. Re-derive the ratio and require it still equals count.
    n = 7;
    if ((int)(sizeof a / sizeof a[0]) != count) {
        return 2;                // strict pin: size not frozen at decl => harness RED
    }
    return count;                // 42
}
