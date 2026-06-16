/* D-CSUBSET-CHAR-STRING-VALUE-CODEGEN + D-CSUBSET-CHAR-INT-WIDENING:
 * end-to-end runtime witness for `char` VALUE codegen — the byte memory
 * ops + the bidirectional char<->int conversions — on EVERY target.
 *
 *   p   = "Az"        a string literal in rodata (GlobalAddr + load base)
 *   c   = *p          BYTE LOAD of 'A' (x86 movzx r/m8 / arm64 LDURB) — a
 *                     1-byte read; a 64-bit load would over-read 7 bytes
 *   neg = (char)200   int->char TRUNC: 200 truncates to the signed char -56
 *   isneg = neg < 0   char->int promotion is a SIGN-extend (x86 movsx r/m8
 *                     / arm64 SXTB): -56 < 0 == 1. A ZERO-extend bug reads
 *                     200, so 200 < 0 == 0 — the exit drops 132 -> 131.
 *   d   = c + 1       char arith: byte-load c, promote (SExt), add, then
 *                     TRUNC the int result back into the char `d` (66)
 *   return c + d + isneg  == 65 + 66 + 1 == 132
 *
 * Also a baseline/optimized DIFFERENTIAL: in the unoptimized pipeline the
 * char locals live in stack slots, so c/d/neg round-trip through a BYTE
 * STORE (x86 mov r/m8,r8 / arm64 STURB) + byte load; Mem2Reg promotes them
 * to registers in the optimized arm — both must yield 132.
 *
 * RED-ON-DISABLE: revert the char->int SExt byte form to a zero-extend and
 * `isneg` flips to 0 -> exit 131 (loud). Revert the byte LOAD to a 64-bit
 * load and a char at a section/page edge faults. 132 is off the smoke-pin
 * value 42 so attribution falls on char codegen. */
int main(void) {
    char* p = "Az";
    char c = *p;
    char neg = (char)200;
    int isneg = neg < 0;
    char d = c + 1;
    return c + d + isneg;
}
