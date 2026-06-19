// FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE) + FC7 C3 (x8 sret) COMPOSITION witness:
// an AAPCS64 variadic function that ALSO returns a >16B struct. This is the end-to-end
// runtime proof that the incoming x8 indirect-result (sret) pointer SURVIVES the
// variadic prologue.
//
// `make(int n, ...)` returns `Big` (24B = three longs > 16B), so AAPCS64 returns it BY
// REFERENCE: the caller allocates the result buffer and passes its address in the
// dedicated x8 register; the callee writes the struct through x8 and returns void. But
// `make` is ALSO variadic, so its prologue must reserve + spill the GR/VR register-save-
// area. That spill materializes the save-area base with `add <scratch>, sp, #off` (the
// save area is the topmost frame zone, beyond STUR's imm9 ±256 reach). If the scratch
// picker landed on x8 (the first caller-saved GPR after the arg GPRs x0..x7), that `add`
// would CLOBBER the incoming sret pointer BEFORE the callee's `read_indirect_result`
// reads it — silently writing the struct result into the callee's own frame instead of
// the caller's buffer. The fix adds x8 (cc.indirectResultRegister) to the scratch
// avoid-set so the picker chooses x9.
//
// make(3, 10, 20, 30): s = 10+20+30 = 60, n = 3 → r.a=60, r.b=3, r.c=63.
// main returns (int)(b.a + b.b + b.c) = 60 + 3 + 63 = 126 → exit 126.
//
// RED-ON-DISABLE: with scratch=x8, the `add` overwrites the sret pointer and `make`
// writes `r` to a stray frame address; main then reads an uninitialized `b` and the
// exit drifts off 126 (typically a SIGSEGV or garbage). The typedef return type is the
// reachable form (a top-level `struct Tag` return type is a pre-FC4 grammar residue) —
// identical ABI codegen. Runs under qemu-aarch64 (the linux-arm64 CI leg / local qemu).

typedef struct { long a; long b; long c; } Big;

Big make(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long s = 0;
    for (int i = 0; i < n; i = i + 1) {
        s = s + va_arg(ap, long);
    }
    va_end(ap);
    Big r;
    r.a = s;
    r.b = n;
    r.c = s + n;
    return r;
}

int main(void) {
    Big b = make(3, 10L, 20L, 30L);
    return (int)(b.a + b.b + b.c);
}
