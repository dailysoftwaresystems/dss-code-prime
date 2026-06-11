// D-CSUBSET-DIVISION-OP-CODEGEN end-to-end pin (cycle 10r,
// 2026-06-04). First runnable example exercising the MIR
// SDiv → LIR cqo + idiv_op → x86 `48 99 48 F7 /7` pipeline
// end-to-end through the linker into a runnable PE/ELF/Mach-O.
//
// **Fixture shape — ConstFold-resistant by construction**: the
// division operands are received as function args, so ConstFold
// (which only folds operations on Const MIR values) cannot see
// the literal 100 / 7 inside `divide()`. The 100 and 7 reach
// `divide` only at runtime via the SysV/MS argument registers
// (rdi/rsi or rcx/rdx). This preserves a live IDIV through the
// optimized pipeline on both arms.
//
// **No C-UB**: 100 / 7 with both operands positive (NOT div-by-
// zero, NOT INT_MIN/-1). The IDIV instruction's `#DE` trap is a
// separate concern (D-OPT6-LICM-TRAP-SAFE-HOIST) this fixture
// must NOT engage.
//
// **REX-overlap regression**: the cycle-10r split (cqo + idiv_op,
// each a single instruction with auto-REX) replaces the cycle-10q
// compound (which packed both into one opcode with embedded REX
// bytes, breaking REX.B for high-reg divisors). If the regalloc
// picks R8-R15 for the divisor + the REX overlap bug were present,
// the divisor would decode as the wrong register → STATUS_INTEGER_
// DIVIDE_BY_ZERO trap (exit code 0xC0000094 on Win32). The byte-
// pin tests in `tests/asm/test_asm_x86_variable.cpp` directly
// assert the byte sequences for both low-reg (RAX) and high-reg
// (R8/R14/R15) divisors; this corpus is the end-to-end gate
// verifying the full pipeline produces a binary that returns 14.

int divide(int a, int b) {
    return a / b;
}

// HIGH-PRESSURE arm (D-LIR-REGALLOC-PRESSURED-IMPLICIT-CLOBBER-PIN
// closure, 2026-06-11 — the shift_ops pressured() precedent applied
// to the div family): 10 locals (a0..a9) + q + x + n all live ACROSS
// a runtime division drain the GPR pool, forcing the allocator's
// hand toward the {rdx, rcx, rax} tail of the free-list pop order at
// the covering ranges' allocations. The x86 realization (cqo +
// idiv_op) declares implicitRegisters (inputs ∪ clobbered =
// {rax, rdx}); only the regalloc's covered-position exclusion
// (implicitClobbersCrossedBy) keeps live ranges off those registers.
// With the exclusion disabled (verified empirically, 2026-06-11, PE
// x86_64): in the BASELINE arm the covering ranges are the locals'
// ALLOCA ADDRESSES (mem2reg only runs optimized) — the 10th address
// vreg (&a9) lands on RDX, the division's pre-op sign-extend (CDQ —
// the 32-bit width-variant of cqo) zeroes RDX (sign-ext of positive
// EAX, and the 32-bit write zero-extends), and the post-division
// reload `mov rdx, [rdx]` NULL-DEREFS — deterministic
// STATUS_ACCESS_VIOLATION (0xC0000005) instead of exit 14, three
// consecutive runs identical. (This example declares NO
// optimizedPipelines arms — and an optimized pipeline would fold the
// literal call chain to a constant anyway [inline + ConstFold,
// verified empirically: still 14 with the exclusion disabled] — so
// the BASELINE arm carries the runtime pressure witness, exactly the
// shift_ops discipline.) A covering value on RAX is
// overwritten by the dividend pin `mov rax, x` BEFORE the compound
// op reads it — the unit sweep (tests/lir/test_lir_regalloc.cpp
// PressuredDivCoveringVregsExcludeImplicitInputAndClobberSet)
// witnesses both ordinals. arm64 is
// structurally immune (native 3-op SDIV, no implicit registers) —
// its arms re-run as regression proof. Fold resistance: x/n arrive
// as function args, so the baseline arm keeps the runtime division
// live.
int pressured(int x, int n) {               // called as pressured(100, 7)
    int a0 = x + 1;                         // 101
    int a1 = x + 2;                         // 102
    int a2 = x + 3;                         // 103
    int a3 = x + 4;                         // 104
    int a4 = x + 5;                         // 105
    int a5 = x + 6;                         // 106
    int a6 = x + 7;                         // 107
    int a7 = x + 8;                         // 108
    int a8 = x + 9;                         // 109
    int a9 = x + 10;                        // 110   (a0..a9 sum = 1055)
    int q = x / n;                          // 100/7 = 14 (runtime IDIV)
    return q + x + n + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8
             + a9 - 1162;                   // 14+100+7+1055-1162 = 14
}

int main() {
    return divide(100, 7) + pressured(100, 7) - 14;  // 14+14-14 = 14
}
