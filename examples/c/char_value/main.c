/* D-CSUBSET-CHAR-STRING-VALUE-CODEGEN + D-CSUBSET-CHAR-INT-WIDENING:
 * end-to-end runtime witness for `char` VALUE codegen — the byte memory
 * ops + the bidirectional char<->int conversions — on EVERY target.
 *
 * PER-PLATFORM `char` SIGNEDNESS (D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET;
 * D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM, CLOSED): `neg=(char)200` has a
 * TARGET-defined value, and the target half that decides is the PLATFORM, not
 * the processor. C 6.2.5p15 leaves bare-`char` signedness
 * IMPLEMENTATION-DEFINED and AAPCS64 leaves it implementation-defined too; the
 * AArch64/GNU-Linux platform ABI chose UNSIGNED, Apple's arm64 platform ABI
 * chose SIGNED, and clang follows the PLATFORM. Bare-`char` signedness is
 * therefore an (architecture × PLATFORM) property, never an architecture
 * property. So 131 is the correct answer on arm64-LINUX ONLY — on arm64-DARWIN
 * the correct answer is 132, the same as on x86_64.
 *
 * EVERY ROW IN expected.json NOW ASSERTS REAL BEHAVIOUR. The arm64:macho64 row
 * used to be pinned at 131 as the closing witness for
 * D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM (DSS zero-extended a negative bare
 * `char` there where clang sign-extends — a silent wrong-value miscompile with
 * no diagnostic, no link error and no crash). TF-C75 closed that anchor and the
 * row was flipped 131 -> 132 as the proof.
 *
 * HOW DSS GETS IT RIGHT (TF-C75): the signedness DSS applies is resolved per
 * (architecture × OBJECT-FORMAT) pair from the target configuration, rather than
 * per architecture — that resolution IS the content of TF-C75. The resolved
 * answer is UNSIGNED on arm64 × elf and SIGNED on every other shipped leg.
 * ★ THIS EXAMPLE DELIBERATELY DOES NOT NAME THE CONFIG KEYS THAT CARRY IT. The
 * declaration has already been reshaped once and may be reshaped again; this
 * example must keep asserting the same OBSERVABLE behaviour across any such
 * rework, so it pins the resolved ANSWER and never the spelling of the key that
 * produces it. So arm64:elf64 (131) and arm64:macho64 (132) are ONE CPU on TWO
 * platforms and are SUPPOSED to differ — if those two rows ever read the same
 * number, this example has stopped testing the thing it exists to test.
 *
 * MEASURED BY THIS AGENT 2026-07-28 on this host (Darwin 25.5.0 / arm64,
 * Apple clang 21.0.0 / clang-2100.1.1.101, /usr/bin/clang), exit codes captured
 * DIRECTLY, never after a pipe:
 *   * clang -dM -E -x c /dev/null -target aarch64-linux-gnu DEFINES
 *     __CHAR_UNSIGNED__ 1; -target arm64-apple-darwin does NOT define it, nor
 *     do x86_64-unknown-linux-gnu, x86_64-pc-windows-msvc, x86_64-apple-darwin.
 *   * _Static_assert((char)-1 < 0) compiles clean (rc 0 => SIGNED) for
 *     arm64-apple-darwin, x86_64-unknown-linux-gnu and x86_64-pc-windows-msvc,
 *     and FAILS (rc 1 => UNSIGNED) for aarch64-linux-gnu.
 *   * THIS example's exact expression, 65 + 66 + ((char)200 < 0), constant-
 *     evaluates to 132 for arm64-apple-darwin, x86_64-unknown-linux-gnu and
 *     x86_64-pc-windows-msvc, and to 131 for aarch64-linux-gnu.
 *   * clang on THIS file emits SIGN-extending byte loads for the char locals on
 *     native arm64-darwin (ldrsb x4, ldrb x1) and ZERO-extending ones under
 *     --target=aarch64-linux-gnu (ldrsb x0, ldrb x5).
 *   * THIS file, clang-built and run natively -> exit 132 at -O0 and 132 at -O2.
 *   * THIS file, DSS-built for arm64:macho64-arm64-darwin-exec and run -> exit
 *     132 in the baseline arm and 132 under the `full-release-like` optimized
 *     arm. DSS and clang now agree on this host.
 * Only the arm64:macho64 leg actually EXECUTES on a darwin host; the other three
 * are compiled and then skipped by `runOn`, and their values rest on the clang
 * facts above.
 *
 *   p   = "Az"        a string literal in rodata (GlobalAddr + load base)
 *   c   = *p          BYTE LOAD of 'A' (x86 movzx r/m8 / arm64 LDURB) — a
 *                     1-byte read; a 64-bit load would over-read 7 bytes
 *   neg = (char)200   int->char TRUNC to a bare `char` (the byte 0xC8). Its
 *                     VALUE is target-defined: -56 where bare `char` is SIGNED
 *                     (x86_64 elf64/pe64 AND arm64-DARWIN), 200 where it is
 *                     UNSIGNED (arm64-LINUX) — see the per-platform note above.
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
 * ★ THE OPTIMIZED ARM IS LOAD-BEARING, NOT DECORATIVE. Unlike the sibling
 * char_signedness, this file has NO `volatile` seed: `(char)200 < 0` is a
 * compile-time-constant expression, and the optimized arm runs ConstFold. That
 * both arms return 132 on arm64-darwin is therefore a real result about MIR
 * ConstFold — it does not fold the SExt/ZExt, so the char compare survives to
 * machine code and the codegen answer stands — and NOT a tautology. If ConstFold
 * ever learns to fold char extensions it must fold them with the RESOLVED
 * signedness; this arm is what catches it doing otherwise. The const-eval twin
 * is tracked by D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS.
 *
 * RED-ON-DISABLE: on the signed-char targets (x86_64 elf64/pe64 AND
 * arm64-darwin) reverting the char->int SExt byte form to a zero-extend flips
 * `isneg` 1 -> 0 and the exit drops 132 -> 131 (loud). Making the target
 * configuration resolve arm64 × macho to UNSIGNED again does the same thing to
 * the macho leg alone, silently and with no diagnostic, which is exactly what
 * that row now guards. On arm64-LINUX 131 is the CORRECT answer
 * (unsigned char there); reverting that leg's promotion to SExt wrongly yields
 * 132 — this example is the per-target signedness witness for
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
