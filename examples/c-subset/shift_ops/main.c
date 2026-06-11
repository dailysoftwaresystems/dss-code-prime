// FC3.5 sweep-c1 — SHIFTS end-to-end (closes the
// D-CSUBSET-32BIT-ALU-FORMS shifts residue).
//
// First runtime witness for `<<` / `>>`: before this cycle no target
// declared ANY shift encoding (A_NoEncodingDeclared at the
// assembler). The realization matrix this program walks:
//
//   x86_64: variable counts via the IMPLICIT-CL role contract
//           (mov rcx, count ; SHL/SAR/SHR r/m, CL = D3 /4 /7 /5) —
//           the FC1 idiv "dividend" pattern with role "count";
//           constant counts via the imm8 forms (C1 /4 /7 /5 ib).
//           Both at BOTH widths (REX.W vs no-REX.W per the FC3-c2
//           width axis — `int` shifts compute 32-wide, `unsigned
//           long long` 64-wide).
//   arm64:  native 3-address LSLV/ASRV/LSRV (X- and W-forms);
//           constant counts ride the same V-forms with the count
//           materialized (the LSL-imm UBFM alias is a deferred
//           peephole).
//
// Discriminators baked into the arithmetic:
//   * `(0 - 64) >> 4` MUST be -4 (arithmetic/sign-fill). HONEST REACH
//     (audit-residue sweep c2, D-AUDIT-WITNESS-STRENGTHENING): a
//     logical-shift misroute yields 0x0FFFFFFC — huge positive, but
//     its ADDITIVE delta vs -4 is exactly 2^28 ≡ 0 mod 256, so the
//     SUM-routed exit aliases back to 42 under POSIX WEXITSTATUS;
//     this term discriminates on Windows (full 32-bit exit) only.
//     The every-leg runtime guarantee is the BRANCH-observed
//     sar_branch() arm below; the encodings themselves are byte-
//     pinned at the asm tier.
//   * `u >> (n + 5)` over unsigned MUST zero-fill at 32 bits.
//   * swapped operand wiring (count<->value) or a count-pin landing
//     on the wrong register derails every term.
//
// C semantics: every count is in [0, width) — counts >= width are UB
// (C23 6.5.7p3; hardware masks, nothing emitted). Negative >> is
// implementation-defined; DSS defines sign-fill (SAR/ASRV) on every
// target. Fold resistance: x/n arrive as FUNCTION ARGS, so the
// baseline arm keeps live runtime shifts; every literal fits the
// arm64 MOVZ imm16 wall.

int shifty(int x, int n) {                  // called as shifty(5, 3)
    int a = x << n;                         // 5<<3  = 40  (var, 32-bit)
    int b = (x << 4) >> n;                  // 80>>3 = 10  (imm shl + var sar)
    int c = (0 - 64) >> 4;                  // -64>>4 = -4 (imm SAR witness)
    unsigned int u = 4096u;
    unsigned int v = u >> (n + 5);          // 4096>>8 = 16 (var SHR, 32-bit)
    unsigned long long w = 160ULL;
    unsigned long long y = w >> (n - 1);    // 160>>2 = 40 (var SHR, 64-bit)
    return a + b + c + (int)v - (int)(y - 20ULL);  // 40+10-4+16-20 = 42
}

// HIGH-PRESSURE arm (FC3.5 sweep-c1 CRITICAL regalloc fix witness):
// 10 locals (a0..a9) + s + x + n all live ACROSS a variable-count shift drain the
// GPR pool, forcing the allocator's hand at the shift RESULT's
// allocation. Pre-fix, the result landed on the implicit count
// register (x86 RCX — near the end of the caller-saved LIFO order on
// both SysV and ms_x64): the 2-addr legalize's `mov result, value`
// then OVERWROTE the role-pinned count, so the shift computed
// `x << (x & 31)` = 5<<5 = 160 instead of `x << n` = 5<<3 = 40 —
// exit 162 instead of 42 (silent miscompile; the regalloc's
// result-def implicit-register exclusion now forbids result==RCX).
// arm64 is structurally immune (native 3-op LSLV, no implicit
// registers) — its arms re-run as regression proof. Fold resistance:
// x/n arrive as function args, so the baseline arm keeps the runtime
// shift live.
int pressured(int x, int n) {               // called as pressured(5, 3)
    int a0 = x + 1;                         //  6
    int a1 = x + 2;                         //  7
    int a2 = x + 3;                         //  8
    int a3 = x + 4;                         //  9
    int a4 = x + 5;                         // 10
    int a5 = x + 6;                         // 11
    int a6 = x + 7;                         // 12
    int a7 = x + 8;                         // 13
    int a8 = x + 9;                         // 14
    int a9 = x + 10;                        // 15   (a0..a9 sum = 105)
    int s = x << n;                         // 5<<3 = 40 (160 if count clobbered)
    return s + x + n + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8
             + a9 - 111;                    // 40+5+3+105-111 = 42
}

// BRANCH-observed SAR witness (audit-residue sweep c2,
// D-AUDIT-WITNESS-STRENGTHENING): collapses the SAR-vs-SHR divergence
// into a SIGN TEST instead of an additive delta, so it discriminates
// on EVERY leg (the shifty() sum-term aliases mod 256 — above).
// x = -64 and n = 3 arrive as runtime args (fold-resistant):
//   arithmetic  x >> n = -8           -> negative -> 42-path
//   logical SHR/LSRV   = 0x1FFFFFF8   -> positive -> exit 7
// The nested probe repeats the test on the imm8-count form
// (C1 /7 ib; arm64 materialized count): -64 >> 4 = -4 vs 0x0FFFFFFC.
// 42 vs 7 (delta 35 ≢ 0 mod 256) survives WEXITSTATUS on every leg.
int sar_branch(int x, int n) {              // called as sar_branch(0 - 64, 3)
    if ((x >> n) < 0) {                     // variable-count SAR (D3 /7 CL; ASRV)
        if ((x >> 4) < 0) {                 // imm8-count SAR (C1 /7 ib)
            return 42;
        }
    }
    return 7;
}

int main() {
    return shifty(5, 3) + pressured(5, 3) + sar_branch(0 - 64, 3)
             - 84;                          // 42+42+42-84 = 42
}
