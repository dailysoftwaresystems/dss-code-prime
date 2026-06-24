// D-CSUBSET-INT-CROSS-SIGNEDNESS-CONVERT witness: the C 6.3.1.3 / 6.5.16.1 signed<->unsigned
// implicit ASSIGNMENT conversion across init / call-arg / return contexts — plus the high-bit
// reinterpret and the U64->I32 narrowing truncation. Every value rides a MUTABLE GLOBAL so the
// load is opaque to ConstFold/Mem2Reg (the conversion can't be folded away). Exit 42 iff EVERY
// conversion is correct; a wrong one flips a check -> 7.
//
// The GATED contexts (init, call-arg, return) are the `isAssignable`-checked sites the
// `intCrossSignednessConverts` opt-in admits. The bare assignment STATEMENT (`b = gu;`) is
// NOT an isAssignable site: its width-exact Cast is materialized gate-independently by the HIR
// `coerce()` arithmetic-core arm (correct C 6.3.1.3 behavior, witnessed here too) — so `b` is
// a coerce-path witness, not a gate witness.
//
// RED-ON-DISABLE: revert the `intCrossSignednessConverts` gate (semantic_config /
// c-subset.lang.json / isAssignable) -> every GATED `int = <unsigned>` (init/call-arg/return)
// rejects with S_TypeMismatch (S0003) -> the example NO LONGER COMPILES (the gate is exactly
// what admits them). The narrowing arm (`int nar = gnar`) is also VALUE-divergent: a wrong
// U64->I32 lowering (no truncation) would not leave 42.
//
// Runs on x86_64 (PE + ELF) and arm64 (ELF, qemu) — the conversion is data-model-agnostic
// (`unsigned long long` is U64 on every model, so the narrowing is real everywhere).

unsigned           gu   = 42;                     // 42 — used in every context below
unsigned           ghi  = 0xFFFFFFFFu;            // -1 when reinterpreted as int (high bit)
unsigned long long gnar = 0x100000000ULL | 0x2A;  // low 32 bits = 0x2A = 42 (U64->I32 narrow)

int take_int(int x) { return x; }                 // int param  <- unsigned arg (call-arg)
int ret_cross(void) { return gu; }                // return     <- unsigned (return ctx)

int main(void) {
    int a = gu;            // init context:       U32 -> I32
    int b;
    b = gu;               // assignment STMT:     U32 -> I32 (gate-independent coerce path)
    int c = take_int(gu);  // call-arg context:   U32 arg -> int param
    int d = ret_cross();   // return context (in ret_cross): U32 -> I32 result
    int neg = ghi;         // high-bit reinterpret: 0xFFFFFFFF -> -1
    int nar = gnar;        // narrowing:            U64 -> I32 truncation -> 42
    return (a == 42 && b == 42 && c == 42 && d == 42
            && neg == -1 && nar == 42) ? 42 : 7;
}
