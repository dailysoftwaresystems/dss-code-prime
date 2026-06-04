// D-CSUBSET-DEREF-LOADSTORE-SMOKE (cycle 10h, 2026-06-04):
// Smoke-pin Deref-as-rvalue codegen end-to-end (source → HIR
// Deref → MIR Load → asm mov-from-memory → correct runtime exit).
//
// Pairs with sibling `deref_store` (lvalue side). Before 10h the
// only `*p`-shaped corpus coverage was via param-type spelling
// (e.g., `int* lpWritten` in hello_writefile) — Deref-AS-RVALUE
// at the language level had no compile→run gate. Without that
// gate, the strict-aliasing capstone (cycle 10i,
// D-OPT-ALIAS-ARC-CORPUS-CAPSTONE-STRICT) would conflate a Deref-
// codegen regression with a CSE alias-rule regression.
//
// Shape (deliberately minimal): a one-argument helper whose body
// is exactly `return *p;` — the smallest program that takes a
// `Ptr<I32>` Arg and yields an `I32` Load result back to the
// caller. `main` materializes a local int, takes its address, and
// passes it. The address-of side is already proven by
// hello_writefile's `&written` → WriteFile; this row proves the
// Deref consumes the typed pointer and the loaded value flows out
// through the SysV/ms_x64 return-register convention.
//
// Exit code 42 matches the corpus convention for smoke pins
// (arithmetic / return_42 / subtraction / etc.) so failure
// attribution falls on Deref codegen rather than on a quirky
// witness value.

int read_through(int* p) {
    return *p;
}

int main() {
    int x;
    x = 42;
    return read_through(&x);
}
