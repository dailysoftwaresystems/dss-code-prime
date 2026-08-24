// D-ASM-AARCH64-LARGE-FRAME-IMM12 + FC12-deferral④
// (D-FC12C-AAPCS64-VARIADIC-OVERFLOW-FIXED-STACK-ARGS)
// + D-MIR-VA-OVERFLOW-ARM-DROPS-FIXED-STACK-DISPLACEMENT
// runtime witness: an arm64 variadic callee whose FIXED params
// overflow the 8 integer arg registers onto the incoming stack, AND whose frame is large
// enough that the 9th fixed param's incoming-stack load needs the SCALED imm12 LDR form
// (the byte offset exceeds the unscaled imm9 ±256 reach). This is the case the scalar-
// only / register-only arm64 varargs corpora cannot exercise.
//
// ★ TWO LEGS, TWO va_list STRATEGIES, ONE SOURCE. The 8-GPR argument vocabulary is
// SHARED by AAPCS64 (elf64-aarch64) and Apple arm64 (macho64-arm64-darwin — its target
// row says the general-purpose register vocabulary is identical to AAPCS64), so the
// SAME nine named ints overflow on both legs; the va_list strategy underneath differs:
//   * elf64-aarch64 — `aapcs64_dual_cursor`: after `va_start(ap, i)` all 8 GPRs are
//     consumed by fixed params, so __gr_offs clamps to 0 (NOT < 0) and va_arg(ap,int)
//     takes the __stack arm, reading the FIRST vararg from the overflow area (past the
//     fixed stack arg `i`) and bumping __stack by 8 (the NSAA quantum).
//   * macho64-arm64-darwin — `homogeneous_pointer` + variadicUsesOverflowBase: Apple
//     has NO register-save area and stacks EVERY vararg (variadicArgsAlwaysStack), so
//     `va_list` is a plain pointer anchored directly at the overflow base and va_arg is
//     a linear +8 bump. The named args that FIT stay in x0..x7 and are read via SSA —
//     but `i` did NOT fit, so the anchor must still skip its incoming stack slot.
// BOTH legs therefore need the SAME displacement, and both get it from the SAME
// `VaOverflowArgAreaAddr` payload (`currentFnFixedStackBytes_` = 1 stacked scalar × 8).
// main passes sum9(1..9, 165): a+..+i + v = 45 + 165 = 210 -> exit 210 on both.
//
// ✔MEASURED 2026-08-24 (Apple clang 21.0.0, `-arch arm64`, -O0 and -O2, real Apple
// Silicon): the same nine-named-int shape puts `i` at incoming+0 and anchors va_start at
// incoming+8 (`ldr w8, [x29,#0x10]` / `add x8, x29, #0x18`), and returns 210 — the
// reference agrees with the exit code pinned here, not merely with itself.
//
// FOLD 1 (LOAD-BEARING, corpus red-on-disable): `int pad[80]` (= 320 bytes), written
// once and read into the return, forces totalFrameSize well past 255 so the 9th-param
// load CANNOT fit imm9 and MUST select `load_u`. Without it the frame might land in
// [193,255] (reg-save-area 192 + va_list ~32), the load would fit imm9, and reverting
// the load_u selection would silently still pass on the old path. The fold SURVIVES the
// darwin port and is MORE load-bearing there, not less: Apple's homogeneous_pointer
// strategy spills no register-save area and its `va_list` is one pointer, so the 192+32
// bytes that carried the aapcs64 frame most of the way to 255 are simply absent — the
// 320B pad is the ONLY thing keeping that leg out of imm9 reach. An ARRAY alloca is
// never scalar-promoted (mem2reg refuses array allocas — "promoting would lose memory
// identity"), so the full 320B reservation survives; `pad[0]` is read back into the
// result (netting zero, see below) so the array is also live against DCE. (NB: an array
// alloca survives mem2reg on its own; `volatile` — implemented since c21 — is not needed
// here. Only a pointer-to-volatile POINTEE still fails loud, S_VolatilePointeeNotSupported.)
//
// RED-ON-DISABLE ①: revert the load_u selection in lir_callconv.cpp -> the 9th-param
// load emits the unscaled `load` (LDUR imm9) at an offset > 255 -> the fixed32 encoder
// fails loud (A_ImmediateOperandOutOfRange) and the corpus fails to assemble. The exit
// 210 is the run-witness that the scaled large-frame load threaded end-to-end.
//
// RED-ON-DISABLE ② (darwin leg, D-MIR-VA-OVERFLOW-ARM-DROPS-FIXED-STACK-DISPLACEMENT):
// drop the `currentFnFixedStackBytes_` payload from the HomogeneousPointer /
// variadicUsesOverflowBase branch of `lowerVaStart` — `ap` then anchors AT the named
// stack param `i` instead of past it, va_arg returns 9 rather than 165, and the exit
// becomes 45 + 9 = 54. NOTHING else moves: no diagnostic on either pipeline, and both
// the aarch64-ELF leg and every x86_64 leg stay green, which is precisely why this
// defect shipped. ✔MEASURED 2026-08-24 on the operator's Apple Silicon Mac.
//
// `i_seed` is a MUTABLE GLOBAL so the 9th fixed arg's value is opaque to ConstFold (it
// cannot be propagated into a constant arg slot). Runs under qemu-aarch64 on the
// linux-arm64 CI and natively on the macos leg; the `release` arm re-runs both through
// the shipped optimizer (this defect was present in release too, so a debug-only witness
// would have been half a witness).

int i_seed = 9;   // mutable global -> opaque load (anti-fold)

int sum9(int a, int b, int c, int d, int e, int f, int g, int h, int i, ...) {
    int pad[80];                    // FOLD 1: 320B local -> frame > 255 (array, not SROA'd)
    pad[0] = i;                     // store keeps the alloca + forces the size
    va_list ap;
    va_start(ap, i);
    int v = va_arg(ap, int);        // the FIRST vararg (165) from __stack overflow
    va_end(ap);
    int echo = pad[0];              // read the array back (live against DCE)
    // a+..+i + v + (pad[0] - i) = 45 + 165 + (9 - 9) = 210; the pad term nets to 0
    // (pad[0] was stored from i) but keeps the 320B array referenced.
    return a + b + c + d + e + f + g + h + i + v + echo - i;
}

int main(void) {
    return sum9(1, 2, 3, 4, 5, 6, 7, 8, i_seed, 165);
}
