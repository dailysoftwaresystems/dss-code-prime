/* D-CSUBSET-CHAR-STRING-VALUE-CODEGEN + D-CSUBSET-CHAR-INT-WIDENING:
 * end-to-end runtime witness for `char` VALUE codegen — the byte memory
 * ops + the bidirectional char<->int conversions — on EVERY target.
 *
 *   p   = "Az"        a string literal in rodata (GlobalAddr + load base)
 *   c   = *p          BYTE LOAD of 'A' (x86 movzx r/m8 / arm64 LDURB) — a
 *                     1-byte read; a 64-bit load would over-read 7 bytes
 *   neg = (char)200   int->char TRUNC to a bare `char` (the byte 0xC8). Its
 *                     VALUE is target-defined: -56 on a signed-char target
 *                     (x86_64/pe64), 200 on an unsigned-char target (arm64,
 *                     per the AArch64 ABI — D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET).
 *   isneg = neg < 0   char->int promotion: SExt (x86 movsx r/m8) on signed
 *                     char -> -56 < 0 == 1; ZExt (arm64 uxtb) on unsigned
 *                     char -> 200 < 0 == 0. So isneg == 1 on x86_64, 0 on arm64.
 *   d   = c + 1       char arith: byte-load c ('A'=65, sign-neutral), promote,
 *                     add, then TRUNC the int result back into the char `d` (66)
 *   return c + d + isneg  == 65 + 66 + isneg == 132 (x86_64) / 131 (arm64)
 *
 * Also a baseline/optimized DIFFERENTIAL: in the unoptimized pipeline the
 * char locals live in stack slots, so c/d/neg round-trip through a BYTE
 * STORE (x86 mov r/m8,r8 / arm64 STURB) + byte load; Mem2Reg promotes them
 * to registers in the optimized arm — both must yield 132.
 *
 * RED-ON-DISABLE: on x86_64 (signed char) reverting the char->int SExt byte
 * form to a zero-extend flips `isneg` 1 -> 0 and the exit drops 132 -> 131
 * (loud). On arm64 the CORRECT answer IS 131 (unsigned char); reverting the
 * arm64 promotion back to SExt wrongly yields 132 — this example is the per-
 * target signedness witness for D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET.
 * Revert the byte LOAD to a 64-bit load and a char at a section/page edge
 * faults. Both 131 and 132 are off the smoke-pin value 42 so attribution
 * falls on char codegen. */
int main(void) {
    char* p = "Az";
    char c = *p;
    char neg = (char)200;
    int isneg = neg < 0;
    char d = c + 1;
    return c + d + isneg;
}
