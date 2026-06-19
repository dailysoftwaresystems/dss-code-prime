// FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the FIRST Win64 (ms_x64) variadic-CALLEE
// RUN-witness — `va_start`/`va_arg`/`va_end` over the HomogeneousPointer strategy,
// with BOTH int AND double varargs, run on THIS Windows host as a PE executable.
//
// `sum(int n, ...)` reads `n` (int, double) PAIRS and accumulates `n + sum(ints) +
// sum((int)doubles)`. `main` calls `sum(2, i0, d0, i1, d1)` — 5 args total. Under
// Win64 the first 4 args occupy the contiguous 32-byte HOME space (rcx/rdx/r8/r9
// slots = the caller's shadow space) and the 5th spills to the stack overflow,
// CONTIGUOUS from the home base. This pins the FC12b substrate end-to-end:
//
//   * va_list is a plain `char*` (8B) — the HomogeneousPointer type (NOT SysV's
//     24B __va_list_tag): the BLOCKER-2 type derivation, threaded into analyze().
//   * va_start sets ap = &home[1] (past the 1 fixed named arg `n`) via the new
//     VaHomeArgAreaAddr leaf, materialized at the NO-SHADOW base `totalFrameSize +
//     callPushBytes` (BLOCKER-1): the home space IS the shadow space, so the base
//     must NOT add shadowSpaceBytes or every va_arg reads one slot too far.
//   * va_arg is a LINEAR pointer bump (no diamond) by 8 bytes, crossing
//     home -> overflow uniformly: i0/d0/i1 read from home (rdx/r8/r9 slots), d1
//     reads from the 5th (stack) slot — the contiguous walk handles both.
//   * The prologue spills the named INTEGER arg regs into their home slots (here
//     just rcx=n) — spill-target == va_arg-read-target (the congruence).
//   * FP-dup (PART 5): d0 is a register-resident FP vararg (slot 2 -> xmm2), so the
//     CALLER duplicates it into the matching home GPR r8 via `movq r8, xmm2`; the
//     callee's va_arg(double) reads it from the r8 home slot. d1 is a STACK FP
//     vararg (slot 4) and is NOT duplicated (overflow FP args ride the stack). A
//     missing/wrong dup -> d0 reads register garbage -> wrong total -> wrong exit.
//
// ANTI-FOLD: every operand derives from a MUTABLE GLOBAL loaded at runtime
// (`g_iseed` int + `g_dseed` double) — opaque to ConstFold/Mem2Reg, so the call's
// arg marshaling (incl. the FP-dup) + the va_arg walk run at runtime in BOTH the
// baseline and optimized arms; the values can't collapse to a literal exit.
//
// EXIT ARITHMETIC (g_iseed = 2, g_dseed = 1.0):
//   n  = g_iseed                 = 2
//   i0 = g_iseed + 8             = 10
//   d0 = g_dseed + 2.5           = 3.5   -> (int) 3   (register FP-dup, xmm2->r8)
//   i1 = g_iseed + 18            = 20
//   d1 = g_dseed + 3.5           = 4.5   -> (int) 4   (stack FP vararg, no dup)
//   total = n + i0 + (int)d0 + i1 + (int)d1 = 2 + 10 + 3 + 20 + 4 = 39
// A wrong FP-dup, a shadow-polluted home base, or a broken home->overflow crossing
// flips the total off 39. Runs on the Windows host's differential ctest.

// Mutable global seeds — their runtime loads are opaque to ConstFold/Mem2Reg.
int    g_iseed = 2;
double g_dseed = 1.0;

int sum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int total = n;                 // start from the fixed named arg
    int i = 0;
    while (i < n) {
        int    iv = va_arg(ap, int);
        double dv = va_arg(ap, double);
        total = total + iv + (int)dv;
        i = i + 1;
    }
    va_end(ap);
    return total;
}

int main(void) {
    int    gi = g_iseed;           // runtime-opaque
    double gd = g_dseed;           // runtime-opaque
    return sum(gi, gi + 8, gd + 2.5, gi + 18, gd + 3.5);
}
