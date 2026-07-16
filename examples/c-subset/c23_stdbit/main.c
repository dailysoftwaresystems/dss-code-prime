// FC17.9(b) C23 <stdbit.h> (D-FULLC-STDBIT): end-to-end runtime witness for ALL 14
// type-generic bit operations (N3096 §7.18), exercised across the 4 concrete widths
// (uc/us/ui/ull), the 4-way `_Generic` form, the per-format `_ul` macro, and the
// edge inputs (0, 1, all-ones, single high bit). Each `stdc_*` op is a macro from
// <stdbit.h> that expands to a `__builtin_stdc_<op>_<T>` compiler intrinsic; that
// intrinsic lowers (hir_to_mir) to a width-correct, single-eval, BRANCHLESS
// composition of the 3 hardware bit-count primitives (Popcount/Clz/Ctz) + universal
// ALU verbs — NOT an ordinary call (a compiler intrinsic is not a linkable symbol).
//
// Fold-resistant: every op runs inside a wrapper fn taking the value as an ARG, so
// on the BASELINE pipeline ConstFold never sees a literal; and the composition's
// Clz/Ctz/Popcount cores are deliberately NOT in ConstFold's fold set (like UMulH),
// so even when the RELEASE pipeline inlines the wrappers the ops stay LIVE — the
// lowering is exercised on every arm (baseline + release/shippedPipeline).
//
// DIFFERENTIAL: x86-64 runs native POPCNT/LZCNT/TZCNT; arm64 runs native CLZ +
// RBIT-then-CLZ ctz + SWAR popcount. The SAME exit on both proves the 14 formulas
// are width-correct independent of the primitive realization (a single-arch bug —
// e.g. a 32-vs-64 width confusion or a clz/ctz swap — would change the sum).
//
// ★ EXIT DERIVED BY HAND from C23 §7.18 (independently, NOT from running DSS):
//   [count/index/width ops return unsigned int; has_single_bit → bool;
//    bit_floor/bit_ceil → the argument's type. W = the operand's bit width.]
//    1  count_ones_ull(0xFFFFFFFF00000000)  = popcount(high 32 set)      = 32
//    2  count_zeros_uc(0xF0)                = 8 − popcount(0b11110000)   = 4
//    3  leading_zeros_uc(0x01)              = 8-bit: 0b00000001          = 7
//    4  leading_ones_us(0xFF00)             = 16-bit: top 8 bits set     = 8
//    5  trailing_zeros_ui(0x00010000)       = bit 16 set                 = 16
//    6  trailing_ones_uc(0x0F)              = 0b00001111                 = 4
//    7  first_leading_zero_uc(0xF0)         = leading_ones 4 → 4+1       = 5
//    8  first_leading_one_uc(0x01)          = leading_zeros 7 → 7+1      = 8
//    9  first_trailing_zero_uc(0x0F)        = trailing_ones 4 → 4+1      = 5
//   10  first_trailing_one_uc(0x08)         = trailing_zeros 3 → 3+1     = 4
//   11  has_single_bit_uc(0x80)             = exactly one bit            = 1
//   12  bit_width_us(0x8000)                = 16-bit: bit 15 → width     = 16
//   13  bit_floor_uc(6)                     = largest pow2 ≤ 6           = 4
//   14  bit_ceil_uc(5)                      = smallest pow2 ≥ 5          = 8
//   15  leading_zeros_uc(0x00)   [edge 0]   = W                         = 8
//   16  trailing_ones_uc(0xFF)   [all-ones] = W (all bits 1)            = 8
//   17  bit_ceil_uc(0)           [edge 0]   = 1                         = 1
//   18  bit_ceil_uc(1)           [edge 1]   = 1                         = 1
//   19  bit_floor_uc(0)          [edge 0]   = 0                         = 0
//   20  has_single_bit_uc(0x06)  [two bits] = 0                         = 0
//   21  count_ones_uc(0xFF)      [all-ones] = 8                         = 8
//   22  stdc_count_ones(ui 0xFF) [generic]  = popcount(255) via _ui     = 8
//   23  stdc_bit_width(ull 0xFF) [generic]  = bit_width(255) via _ull   = 8
//   24  stdc_count_ones_ul(0x0F) [per-fmt]  = 4 on BOTH LP64 & LLP64    = 4
//   SUM = 32+4+7+8+16+4+5+8+5+4+1+16+4+8 + 8+8+1+1+0+0+8 + 8+8+4 = 168.
// Every summed term is C23-DEFINED (no overflow-UB input); the bit_ceil overflow
// clamp is covered by the red-on-disable MIR shift-clamp pin, not this sum.

#include <stdbit.h>

typedef unsigned char      uc;
typedef unsigned short     us;
typedef unsigned int       ui;
typedef unsigned long      ul;
typedef unsigned long long ull;

static unsigned co_ull(ull x) { return stdc_count_ones_ull(x); }
static unsigned cz_uc(uc x)   { return stdc_count_zeros_uc(x); }
static unsigned lz_uc(uc x)   { return stdc_leading_zeros_uc(x); }
static unsigned lo_us(us x)   { return stdc_leading_ones_us(x); }
static unsigned tz_ui(ui x)   { return stdc_trailing_zeros_ui(x); }
static unsigned to_uc(uc x)   { return stdc_trailing_ones_uc(x); }
static unsigned flz_uc(uc x)  { return stdc_first_leading_zero_uc(x); }
static unsigned flo_uc(uc x)  { return stdc_first_leading_one_uc(x); }
static unsigned ftz_uc(uc x)  { return stdc_first_trailing_zero_uc(x); }
static unsigned fto_uc(uc x)  { return stdc_first_trailing_one_uc(x); }
static unsigned hsb_uc(uc x)  { return stdc_has_single_bit_uc(x); }
static unsigned bw_us(us x)   { return stdc_bit_width_us(x); }
static unsigned bf_uc(uc x)   { return stdc_bit_floor_uc(x); }
static unsigned bc_uc(uc x)   { return stdc_bit_ceil_uc(x); }
static unsigned co_uc(uc x)   { return stdc_count_ones_uc(x); }
static unsigned gco(ui x)     { return stdc_count_ones(x); }      // generic → _ui
static unsigned gbw(ull x)    { return stdc_bit_width(x); }       // generic → _ull
static unsigned co_ul(ul x)   { return stdc_count_ones_ul(x); }   // per-format _ul

int main(void) {
    unsigned total = 0;
    total += co_ull(0xFFFFFFFF00000000ull);  // 32
    total += cz_uc(0xF0);                     // 4
    total += lz_uc(0x01);                     // 7
    total += lo_us(0xFF00);                   // 8
    total += tz_ui(0x00010000u);              // 16
    total += to_uc(0x0F);                     // 4
    total += flz_uc(0xF0);                    // 5
    total += flo_uc(0x01);                    // 8
    total += ftz_uc(0x0F);                    // 5
    total += fto_uc(0x08);                    // 4
    total += hsb_uc(0x80);                    // 1
    total += bw_us(0x8000);                   // 16
    total += bf_uc(6);                        // 4
    total += bc_uc(5);                        // 8
    total += lz_uc(0x00);                     // 8   (edge: 0 → W)
    total += to_uc(0xFF);                     // 8   (all-ones guard)
    total += bc_uc(0);                        // 1   (edge: 0 → 1)
    total += bc_uc(1);                        // 1   (edge: 1 → 1)
    total += bf_uc(0);                        // 0   (edge: 0 → 0)
    total += hsb_uc(0x06);                    // 0   (two bits → not single)
    total += co_uc(0xFF);                     // 8   (all-ones)
    total += gco(0xFFu);                      // 8   (generic → _ui)
    total += gbw(0xFFull);                    // 8   (generic → _ull)
    total += co_ul(0x0Ful);                   // 4   (per-format _ul)
    return (int)total;                        // 168
}
