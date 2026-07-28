/* D-CSUBSET-CHAR-STRING-VALUE-CODEGEN + D-CSUBSET-CHAR-INT-WIDENING:
 * end-to-end runtime witness for `char` VALUE codegen — the byte memory
 * ops + the bidirectional char<->int conversions — on EVERY target.
 *
 * ⚠ ABI CORRECTION (D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET): an earlier
 * revision of this header said `neg=(char)200` is 200 "on arm64, per the
 * AArch64 ABI", and that "On arm64 the CORRECT answer IS 131 (unsigned
 * char)". BOTH ARE FALSE AS STATED, and the second is exactly what let the
 * arm64:macho64 row in expected.json read as if it asserted correct
 * behaviour. What is actually true: AAPCS64 leaves bare-`char` signedness
 * IMPLEMENTATION-DEFINED. The AArch64/GNU-Linux platform ABI chose UNSIGNED;
 * Apple's arm64 platform ABI DIVERGED and chose SIGNED; and clang follows the
 * PLATFORM, not the architecture. Bare-`char` signedness is therefore an
 * (architecture × PLATFORM) property, never an architecture property. So 131
 * is the correct answer on arm64-LINUX ONLY — on arm64-DARWIN the correct
 * answer is 132, the same as on x86_64.
 *
 * MEASURED BY THIS AGENT 2026-07-28 on this host (Darwin 25.5.0 / arm64,
 * Apple clang 21.0.0 / clang-2100.1.1.101), exit codes captured DIRECTLY,
 * never after a pipe:
 *   * clang -dM -E -x c /dev/null -target aarch64-linux-gnu DEFINES
 *     __CHAR_UNSIGNED__ 1; -target arm64-apple-darwin does NOT define it, nor
 *     do x86_64-unknown-linux-gnu or x86_64-pc-windows-msvc.
 *   * _Static_assert((char)-1 < 0) compiles clean (rc 0) for
 *     arm64-apple-darwin, x86_64-unknown-linux-gnu and x86_64-pc-windows-msvc,
 *     and FAILS (rc 1) for aarch64-linux-gnu.
 *   * THIS example's exact expression, 65 + 66 + ((char)200 < 0), constant-
 *     evaluates to 132 for arm64-apple-darwin, x86_64-unknown-linux-gnu and
 *     x86_64-pc-windows-msvc, and to 131 for aarch64-linux-gnu (clang note:
 *     "expression evaluates to '131 == 132'").
 *   * clang on THIS file emits `ldrsb` (sign-extend) for the char locals on
 *     native arm64-darwin and `ldrb` (zero-extend) under
 *     --target=aarch64-linux-gnu.
 *   * THIS file, clang-built and run natively -> exit 132 at -O0 and 132 at -O2.
 *   * THIS file, DSS-built for arm64:macho64-arm64-darwin-exec and run -> exit
 *     131 in debug and 131 under --config=release.
 *
 * !!! THE arm64:macho64 ROW IN expected.json PINS THAT 132-vs-131 DIVERGENCE AS
 * A KNOWN DEFECT -- IT DOES NOT ASSERT CORRECT BEHAVIOUR. Anchor:
 * D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM (OPEN, CONFIRMED). Root cause:
 * src/dss-config/targets/arm64.target.json declares ONE flat
 * `charIsUnsigned: true` for the whole architecture -- right for
 * aarch64-linux-gnu, wrong for arm64-apple-darwin -- and that key cannot
 * express the (architecture × platform) split. It is a SILENT wrong-value
 * miscompile: no diagnostic, no link error, no crash, just a different number
 * on every negative-`char` path. Clang ground truth on that row is 132; the
 * pinned DSS value is 131. WHEN THE ANCHOR CLOSES THAT EXPECTATION MUST FLIP
 * 131 -> 132 -- the example must not be deleted, relaxed, or weakened to
 * silence a failure here. The full measured record lives in that row's own
 * $comment in expected.json; read it first.
 *
 *   p   = "Az"        a string literal in rodata (GlobalAddr + load base)
 *   c   = *p          BYTE LOAD of 'A' (x86 movzx r/m8 / arm64 LDURB) — a
 *                     1-byte read; a 64-bit load would over-read 7 bytes
 *   neg = (char)200   int->char TRUNC to a bare `char` (the byte 0xC8). Its
 *                     VALUE is target-defined: -56 where bare `char` is SIGNED
 *                     (x86_64 elf64/pe64 AND arm64-DARWIN), 200 where it is
 *                     UNSIGNED (arm64-LINUX) — see the ABI correction above.
 *   isneg = neg < 0   char->int promotion: SExt (x86 movsx r/m8 / arm64 sxtb,
 *                     ldrsb) on signed char -> -56 < 0 == 1; ZExt (x86 movzx /
 *                     arm64 uxtb, ldrb) on unsigned char -> 200 < 0 == 0. So
 *                     isneg == 1 on x86_64 and on arm64-DARWIN, 0 on
 *                     arm64-LINUX.
 *   d   = c + 1       char arith: byte-load c ('A'=65, sign-neutral), promote,
 *                     add, then TRUNC the int result back into the char `d` (66)
 *   return c + d + isneg  == 65 + 66 + isneg == 132 where bare `char` is signed
 *                     (x86_64 elf64/pe64, arm64-darwin) / 131 where it is
 *                     unsigned (arm64-linux)
 *
 * Also a baseline/optimized DIFFERENTIAL: in the unoptimized pipeline the
 * char locals live in stack slots, so c/d/neg round-trip through a BYTE
 * STORE (x86 mov r/m8,r8 / arm64 STURB) + byte load; Mem2Reg promotes them
 * to registers in the optimized arm — both arms must yield the SAME value on
 * a given target (132 on the signed-char targets, 131 on the unsigned-char
 * ones), i.e. optimizing must not change the answer.
 *
 * RED-ON-DISABLE: on the signed-char targets (x86_64 elf64/pe64 today, plus
 * arm64-darwin once the anchor above closes) reverting the char->int SExt byte
 * form to a zero-extend flips `isneg` 1 -> 0 and the exit drops 132 -> 131
 * (loud). On arm64-LINUX 131 is the CORRECT answer (unsigned char there);
 * reverting that leg's promotion to SExt wrongly yields 132 — this example is
 * the per-target signedness witness for
 * D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET. Revert the byte LOAD to a 64-bit
 * load and a char at a section/page edge faults. Both 131 and 132 are off the
 * smoke-pin value 42 so attribution falls on char codegen. */
int main(void) {
    char* p = "Az";
    char c = *p;
    char neg = (char)200;
    int isneg = neg < 0;
    char d = c + 1;
    return c + d + isneg;
}
