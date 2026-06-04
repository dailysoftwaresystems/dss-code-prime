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

int main() {
    return divide(100, 7);
}
