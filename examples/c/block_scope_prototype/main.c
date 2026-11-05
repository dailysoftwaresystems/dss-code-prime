// D-CSUBSET-BLOCK-SCOPE-PROTOTYPE witness: a function prototype declared in BLOCK
// scope (`int odd(int);` inside a function body) has EXTERNAL linkage and REFERS
// to the file-scope function of the same name (C 6.2.2p4 / 6.7.6.3) — it does NOT
// introduce a distinct block-local function that shadows the outer one. Pre-fix
// the block-scope proto bound a separate block-local symbol; a call resolved to
// that bodyless shadow -> H0009 (a Ref to an unbound symbol) at HIR->MIR.
//
// Here `even` block-declares the prototype `int odd(int);` and forward-calls
// `odd` (defined LATER at file scope). The block proto binds in its block and the
// block-extern sweep MERGES it with the file-scope definition (P69 fold f26b; it was
// re-homed onto the file scope before) — the call resolves to the real `odd`. Value-divergent so a shadow/merge bug yields the wrong exit:
//   even(10) = 1, odd(7) = 1  =>  (1 && 1) => 42 ; any miscompile => 7.
//
// RED-ON-DISABLE, as filed: revert the Pass-1 re-home -> the block proto shadows,
// the forward call hits the bodyless shadow -> H0009, the program no longer
// COMPILES. Since P69 fold f26b there is no re-home: the merge is the block-extern
// sweep's, and its red is SemanticAnalyzerC.BlockScopePrototypeMergesWithFileDefinition
// (two surviving functions named `odd`).
//
// Front-end (semantic + HIR) feature, target-agnostic — the emitted code is two
// ordinary mutually-recursive functions. Runs on x86_64 (PE+ELF) and arm64 (ELF
// qemu, Mach-O macos leg).

int even(int n) {
    int odd(int);                      // BLOCK-scope prototype of the file fn
    return n == 0 ? 1 : odd(n - 1);    // forward-calls odd (defined below)
}

int odd(int n) { return n == 0 ? 0 : even(n - 1); }

int main(void) {
    int a = even(10);   // 1
    int b = odd(7);     // 1
    return (a == 1 && b == 1) ? 42 : 7;
}
