/* D-FC7-INDIRECT-X8-SRET-CALLEE-EXCLUSION (AAPCS64 / Apple arm64): an INDIRECT
 * (function-pointer) call returning a >16-byte struct BY VALUE. makeBig() returns
 * a 24B {long;long;long} -> >16B -> the caller allocates the result storage and
 * passes its address in the dedicated x8 indirect-result register; the callee
 * returns VOID (it writes through x8). Called through a FUNCTION POINTER `fp`, the
 * callee address is a regalloc vreg that must avoid x8: the FC7-C3 IRR reroute
 * `mov x8, sretPtr` is materialized POST-regalloc, BETWEEN fp's definition and the
 * call. Because the arg registers x0..x7 are excluded for the indirect callee, the
 * lowest free GPR is x8 (caller-saved, NOT an arg reg) -> before this fix the
 * callee could be parked there and the IRR move would CLOBBER it, tripping the loud
 * L_IndirectCalleeClobberedByArgSetup backstop -> a VALID program failed to COMPILE.
 *
 * Value flows through the non-inlined mkl() so nothing const-folds; the >16B return
 * forces the x8 SRET path. 13 + 14 + 15 = 42.
 *
 * SCOPE: this corpus proves the x8-sret FN-POINTER path WORKS end-to-end (exit 42) —
 * it does NOT itself force the x8 collision. With no register pressure the allocator
 * parks `fp` on a callee-saved register (x29), never x8, so disabling the fix leaves
 * THIS program byte-identical. The RED-ON-DISABLE witness is the host-independent
 * structural pin `LirRegAlloc.Aapcs64PressuredIndirectStructReturnCalleeExcludesX8`
 * (tests/lir/test_lir_regalloc.cpp), which lands the callee ON x8 under a register-
 * pressure sweep the moment the indirect-result exclusion is removed. AAPCS64 runtime
 * closes on arm64-ELF under qemu (Apple Mach-O is the same struct ABI but runs only on
 * the macos CI leg; SysV/Win64 have no indirectResultRegister, so x8 is moot there —
 * this is an AAPCS64/Apple-specific exclusion). */
typedef struct { long a; long b; long c; } Big;

long mkl(long v) { return v; }

Big makeBig(void) {
    Big r;
    r.a = mkl(13);
    r.b = mkl(14);
    r.c = mkl(15);
    return r;
}

int main(void) {
    Big (*fp)(void) = &makeBig;
    Big b = fp();
    return (int)(b.a + b.b + b.c);
}
