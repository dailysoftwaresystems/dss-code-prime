// D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED — `&&label` as a STATIC-DATA
// address constant, the GNU shape both gcc and clang accept.
//
// The sibling example `cross_cu_computed_goto_table/` reaches the same jump-table
// lowering through an ISO-C dense `switch`. This one reaches it through the GNU
// spelling the interpreter idiom actually uses, where the programmer writes the
// table: `static void *tbl[] = {&&L0, &&L1, ...}; goto *tbl[i];`. gcc emits that
// table into `.rdata` as `.quad .L3` — a relocation against an interior block
// label, not a startup store (✔measured, gcc 13.2, `-O0 -S`) — and DSS now does
// the same, which is what this example witnesses.
//
// ★ THE EXIT CODE WITNESSES THE RELOCATION VALUES, NOT MERELY THAT IT LINKED. Each
// arm returns a distinct number, so a table slot bound to the WRONG block (or to a
// stale byte offset after the optimizer moved code) changes the sum. That is the
// failure this feature can have that a build-only check cannot see.

// ── 1. The plain table: three labels, addresses taken ONLY from static data ──
// Nothing in the body materializes these addresses, so no block-address `lea` is
// emitted and the block symbols must be bound from the assembled function's byte
// offsets — the same binding the dense-switch jump table needs.
static int table3(int i) {
    static void *const tbl[] = {&&L0, &&L1, &&L2};
    goto *tbl[i];
L0:
    return 10;
L1:
    return 20;
L2:
    return 30;
}

// ── 2. Trampoline-shaped targets: every label block is a single branch ────────
// An address-taken block that holds only a `goto` is exactly the shape SimplifyCfg
// elides. Eliding one here would leave its symbol bound to code that moved — a
// SILENT wrong branch, not a link error — so this arm is the one that fails if the
// address-taken protection stops covering blocks reached only from data. It is why
// the release pipeline below names `SimplifyCfg`.
static int trampolines(int i) {
    static void *const tbl[] = {&&T0, &&T1, &&T2};
    goto *tbl[i];
T0:
    goto E0;
T1:
    goto E1;
T2:
    goto E2;
E0:
    return 1;
E1:
    return 2;
E2:
    return 4;
}

// ── 3. One label, addressed from BOTH the body and static data ───────────────
// The body's `&&A` materializes the address in a register while the table
// relocates against it, and the two MUST resolve to ONE symbol. Declaring a symbol
// twice is a link error the computed-goto path already hit once, which is why the
// mint is memoized per block rather than per use.
static int shared_symbol(int i) {
    static void *const tbl[] = {&&A, &&A};
    void *body = &&A;
    if (i == 2) goto *body;
    goto *tbl[i];
A:
    return 8;
}

// ── 4. The scalar shape ──────────────────────────────────────────────────────
// `static void *const p = &&L;` — no aggregate involved. It fails differently from
// the array when unsupported (the aggregate path bails to a runtime store-chain;
// the scalar reached the synthesized module-init and aborted there), so both
// shapes are pinned.
static int scalar(int i) {
    static void *const p = &&S;
    if (i) goto *p;
    return 16;
S:
    return 32;
}

// ── 5. Two functions whose labels share ordinal 0 ────────────────────────────
// Label ordinals restart per function, so BOTH of these initializers say "label
// ordinal 0". Resolving them without the owning function would send one function's
// computed goto into the other function's block. Nothing else in this file would
// catch that: each function is individually correct.
static int amb_a(int i) {
    static void *const p = &&X;
    if (i) goto *p;
    return 0;
X:
    return 64;
}
static int amb_b(int i) {
    static void *const p = &&Y;
    if (i) goto *p;
    return 0;
Y:
    return 128;
}

int main(void) {
    int sum = 0;
    if (table3(0) != 10) return 1;
    if (table3(1) != 20) return 2;
    if (table3(2) != 30) return 3;
    sum += table3(2);                 // 30

    if (trampolines(0) != 1) return 4;
    if (trampolines(1) != 2) return 5;
    if (trampolines(2) != 4) return 6;
    sum += trampolines(0) + trampolines(1) + trampolines(2);   // +7 => 37

    if (shared_symbol(0) != 8) return 7;
    if (shared_symbol(1) != 8) return 8;
    if (shared_symbol(2) != 8) return 9;   // reached through the BODY's copy

    if (scalar(0) != 16) return 10;
    if (scalar(1) != 32) return 11;
    sum += scalar(0) - 11;            // +5 => 42

    if (amb_a(0) != 0) return 12;
    if (amb_b(0) != 0) return 13;
    if (amb_a(1) != 64) return 14;
    if (amb_b(1) != 128) return 15;

    return sum;                       // 42
}
