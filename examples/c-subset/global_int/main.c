// D-LK4-RODATA-PRODUCER end-to-end smoke: a module-level `int`
// global with a constant initializer must reach the binary's
// .rdata section, and `main`'s return-of-global must materialize
// the global's address via RIP-relative lea + load through it.
//
// Pipeline exercised: HIR `Global(answer, init=42)` → MIR
// `MirGlobal{int, sym, lit_idx_for_42}` →
// `lowerMirGlobalsToDataItems` materializes `AssembledData{sym,
// Rodata, [42,0,0,0], Alignment{4}}` → PE walker emits the
// .rdata section with those 4 bytes → main's body lowers to
// `lea reg, [rip + sym]` (D-LK4-RODATA-PRODUCER closure of the
// `lea SymbolRef` variant) + `load reg', [reg]` + return.
// Specific register assignments are regalloc-determined; only
// the opcode shape is fixed by the lowerer.
//
// A regression in any tier surfaces as the spawned binary
// returning a value other than 42 — a 1-byte drift in the
// disp32 patch points the load at the wrong address (garbage),
// caught by the harness's strict ASSERT_EQ on the OS exit code.
int answer = 42;

int main() {
    return answer;
}
