// D-ASM-AARCH64-LARGE-FRAME-IMM12 + FC12-deferral④ (D-FC12C-AAPCS64-VARIADIC-OVERFLOW-
// FIXED-STACK-ARGS) runtime witness: an AAPCS64 variadic callee whose FIXED params
// overflow the 8 integer arg registers onto the incoming stack, AND whose frame is large
// enough that the 9th fixed param's incoming-stack load needs the SCALED imm12 LDR form
// (the byte offset exceeds the unscaled imm9 ±256 reach). This is the case the scalar-
// only / register-only AAPCS64 varargs corpora cannot exercise.
//
// `sum9(int a..i, ...)`: AAPCS64 has 8 integer arg registers (x0..x7), so the 8 fixed
// ints a..h occupy x0..x7 and `i` (the 9th fixed param) is passed ON THE INCOMING STACK.
// The callee loads `i` from `[sp + totalFrameSize]`; with the FOLD-1 pad below the frame
// is comfortably > 255, so that load is encoded as `LDR Xt, [sp, #pimm]` (the scaled
// unsigned-offset form, `load_u`) — the unscaled `LDUR` (imm9, ±256) cannot reach it.
// After `va_start(ap, i)`, all 8 GPRs are consumed by fixed params, so __gr_offs clamps
// to 0 (NOT < 0): va_arg(ap, int) takes the __stack arm and reads the FIRST vararg from
// the overflow area (past the fixed stack arg `i`), bumping __stack by 8 (the NSAA
// quantum). main passes sum9(1..9, 165): a+..+i + v = 45 + 165 = 210 -> exit 210.
//
// FOLD 1 (LOAD-BEARING, corpus red-on-disable): `int pad[80]` (= 320 bytes), written
// once and read into the return, forces totalFrameSize well past 255 so the 9th-param
// load CANNOT fit imm9 and MUST select `load_u`. Without it the frame might land in
// [193,255] (reg-save-area 192 + va_list ~32), the load would fit imm9, and reverting
// the load_u selection would silently still pass on the old path. An ARRAY alloca is
// never scalar-promoted (mem2reg refuses array allocas — "promoting would lose memory
// identity"), so the full 320B reservation survives; `pad[0]` is read back into the
// result (netting zero, see below) so the array is also live against DCE. (NB: `volatile`
// is grammar-admitted but fails LOUD in the c-subset — S_VolatileNotSupported — so it
// cannot be used here.)
//
// RED-ON-DISABLE: revert the load_u selection in lir_callconv.cpp -> the 9th-param load
// emits the unscaled `load` (LDUR imm9) at an offset > 255 -> the fixed32 encoder fails
// loud (A_ImmediateOperandOutOfRange) and the corpus fails to assemble. The exit 210 is
// the run-witness that the scaled large-frame load threaded end-to-end. `i_seed` is a
// MUTABLE GLOBAL so the 9th fixed arg's value is opaque to ConstFold (it cannot be
// propagated into a constant arg slot). Runs under qemu-aarch64 on the linux-arm64 CI.

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
