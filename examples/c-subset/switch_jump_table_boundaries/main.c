/* D-OPT-SWITCH-JUMP-TABLE (c70) adversarial boundaries witness. Two DENSE
   switches — both qualify for the O(1) jump-table lowering — probed at every
   control-flow boundary a jump table can get wrong:

     - the FIRST case             (classify(0)   == 100)
     - the LAST case              (classify(9)   == 109)
     - an in-range GAP value      (classify(5)   -> default 999; case 5 omitted)
     - one BELOW the min          (classify(-1)  -> default 999)
     - one ABOVE the max          (classify(10)  -> default 999)
     - far out of range           (classify(1000)-> default 999)

   classify() is a contiguous [0..9] range with a hole at 5. classify_neg() is a
   NEGATIVE-based contiguous range [-4..4] with a hole at 0 — it stresses a
   negative table minimum, a negative discriminant, and the unsigned
   `(unsigned)(disc - min) > (max - min)` bounds trick (which must treat a
   negative min correctly, and must NOT sign-mishandle the index widen — the
   c42 D-CSUBSET-INDEX-NEGATIVE-WIDEN lesson: the table index must be widened to
   64 bits before it scales into the address, or a wrong/huge index SIGSEGVs).

   Every probe returns the RIGHT case's value, never a neighbor's — an off-by-one
   table, a mishandled gap, or a broken bounds check yields a non-42 exit (the
   return code names which probe failed) or SIGSEGVs, never a silent pass.
   gcc/clang compile both switches to a jump table at -O1 and exit 42 (verified:
   x86_64 -O0/-O1 and aarch64 -O1 under qemu).

   RED-ON-DISABLE: force the sparse compare-chain path (disable the dense
   jump-table arm in lowerSwitch) and this still compiles + exits 42 via the
   immediate-compare chain (proving that path also carries the boundaries
   correctly); the jump table and the compare chain must AGREE bit-for-bit. */

int classify(int n) {
    int r = 999;             /* default sentinel — also the gap/out-of-range value */
    switch (n) {
        case 0: r = 100; break;
        case 1: r = 101; break;
        case 2: r = 102; break;
        case 3: r = 103; break;
        case 4: r = 104; break;
        /* case 5 intentionally omitted -> in-range GAP -> default */
        case 6: r = 106; break;
        case 7: r = 107; break;
        case 8: r = 108; break;
        case 9: r = 109; break;
    }
    return r;
}

int classify_neg(int n) {
    int r = 888;
    switch (n) {
        case -4: r = 200; break;
        case -3: r = 201; break;
        case -2: r = 202; break;
        case -1: r = 203; break;
        /* case 0 omitted -> gap -> default */
        case 1: r = 205; break;
        case 2: r = 206; break;
        case 3: r = 207; break;
        case 4: r = 208; break;
    }
    return r;
}

int main(void) {
    if (classify(0)    != 100) return 1;   /* first case            */
    if (classify(9)    != 109) return 2;   /* last case             */
    if (classify(5)    != 999) return 3;   /* in-range gap          */
    if (classify(-1)   != 999) return 4;   /* below min             */
    if (classify(10)   != 999) return 5;   /* above max             */
    if (classify(1000) != 999) return 6;   /* far above             */
    if (classify(4)    != 104) return 7;   /* interior              */
    if (classify(6)    != 106) return 8;   /* interior after gap    */

    if (classify_neg(-4)    != 200) return 20;  /* negative first   */
    if (classify_neg(4)     != 208) return 21;  /* negative last    */
    if (classify_neg(-1)    != 203) return 22;  /* negative interior*/
    if (classify_neg(0)     != 888) return 23;  /* negative gap     */
    if (classify_neg(-5)    != 888) return 24;  /* below neg min    */
    if (classify_neg(5)     != 888) return 25;  /* above neg max    */
    if (classify_neg(-1000) != 888) return 26;  /* far negative     */

    return 42;
}
