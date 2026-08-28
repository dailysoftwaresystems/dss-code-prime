/* CU 2 of `c_inline_asm_crosscu_merge`.
 *
 * This file's ONLY job is to be a SECOND translation unit, because N>=2 is the
 * whole trigger: `mergeCuMirs` runs only for N>=2, and the cross-CU MIR clone is
 * the site that had no `InlineAsm` arm. A single-source example cannot reach it
 * no matter what asm it contains.
 *
 * `dss_seed` is `volatile` so the eight `dssOp` results cannot be constant-folded
 * away at `--config=release` — a folded call is not a call, and the aarch64 pin
 * in `main.c` needs eight CALL-DERIVED values live across the asm block to have
 * anything to lose. */

volatile int dss_seed = 3;

int dssOp(int k) { return dss_seed + k; }
