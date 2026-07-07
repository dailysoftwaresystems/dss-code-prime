/* D-OPT-SWITCH-JUMP-TABLE (c70) SPARSE-path witness. A switch whose case
   values are widely spaced (0, 7, 100, 5000, -3, 123456) so the density test
   (max-min+1 <= K*caseCount) FAILS -> it stays on the compare-chain path. The
   c70 fix makes that path emit each case constant as an IMMEDIATE compare
   (`cmp discrim, imm32`) instead of materializing it into a live register, so
   even a sparse switch no longer piles up per-case vregs. This corpus keeps the
   case set small but adversarial: a negative case, a large-but-int32 case, and
   a default — every arm reached and cross-checked against gcc (exit 42).

   RED-ON-DISABLE (immediate-compare): revert the sparse arm to regForValue +
   cmp reg,reg and this still exits 42 (correctness unchanged) but rebuilds the
   register pressure the fix removed. */

int pick(int n) {
    switch (n) {
        case 0:      return 10;
        case 7:      return 17;
        case 100:    return 110;
        case 5000:   return 5010;
        case -3:     return 7;
        case 123456: return 654321 - 654279;   /* 42 */
        default:     return -1;
    }
}

int main(void) {
    if (pick(0)      != 10)    return 1;
    if (pick(7)      != 17)    return 2;
    if (pick(100)    != 110)   return 3;
    if (pick(5000)   != 5010)  return 4;
    if (pick(-3)     != 7)     return 5;
    if (pick(123456) != 42)    return 6;
    if (pick(1)      != -1)    return 7;   /* no case -> default */
    if (pick(-100)   != -1)    return 8;   /* negative no-case -> default */
    if (pick(999999) != -1)    return 9;   /* far no-case -> default */
    return 42;
}
