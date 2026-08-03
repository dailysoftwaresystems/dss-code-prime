// TF-C56 + TF-C75 (D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET;
// D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM, CLOSED): bare `char`'s signedness is
// TARGET-defined, and it is decided by the (architecture x PLATFORM) pair --
// NEVER by the architecture alone. C 6.2.5p15 leaves it IMPLEMENTATION-DEFINED
// and AAPCS64 leaves it implementation-defined too, so the PLATFORM is what
// decides: the AArch64/GNU-Linux platform ABI chose UNSIGNED, Apple's arm64
// platform ABI chose SIGNED, and clang follows the PLATFORM, not the processor.
// x86_64 (SysV + Windows/pe64) is SIGNED everywhere. A `char` holding the byte
// 0x80 therefore promotes to `int` as -128 wherever `char` is signed (SExt /
// sxtb / ldrsb) and as +128 where it is unsigned (ZExt / uxtb / ldrb), so
// `c < 0` is TRUE on x86_64 AND on arm64-DARWIN, and FALSE on arm64-LINUX.
//
// THIS EXAMPLE ASSERTS REAL BEHAVIOUR ON EVERY LEG -- no row in expected.json
// pins a defect any more. The arm64:macho64 row used to be pinned at 20 as the
// closing witness for D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM (DSS zero-extended
// a negative bare `char` there where clang sign-extends -- a silent wrong-value
// miscompile with no diagnostic). TF-C75 closed that anchor and the row was
// flipped 20 -> 10 as the proof.
//
// HOW DSS GETS IT RIGHT (TF-C75): the signedness DSS applies is resolved per
// (architecture x OBJECT-FORMAT) pair from the target configuration, rather than
// per architecture -- that resolution IS the content of TF-C75. The resolved
// answer is UNSIGNED on arm64 x elf and SIGNED on every other shipped leg.
// ★ THIS EXAMPLE DELIBERATELY DOES NOT NAME THE CONFIG KEYS THAT CARRY IT. The
// declaration has already been reshaped once and may be reshaped again; this
// example must keep asserting the same OBSERVABLE behaviour across any such
// rework, so it pins the resolved ANSWER and never the spelling of the key that
// produces it. So arm64:macho64 (10) and arm64:elf64 (20) are ONE CPU on TWO
// platforms and are SUPPOSED to differ -- if those two rows ever read the same
// number, this example has stopped testing the thing it exists to test.
//
// MEASURED BY THIS AGENT 2026-07-28 on this host (Darwin 25.5.0 / arm64, Apple
// clang 21.0.0 / clang-2100.1.1.101, /usr/bin/clang), exit codes captured
// DIRECTLY, never after a pipe:
//   * clang -dM -E -x c /dev/null -target aarch64-linux-gnu  DEFINES
//     __CHAR_UNSIGNED__ 1;  -target arm64-apple-darwin does NOT define it, nor
//     do x86_64-unknown-linux-gnu, x86_64-pc-windows-msvc, x86_64-apple-darwin.
//   * _Static_assert((char)-1 < 0) compiles clean (rc 0 => SIGNED) for
//     arm64-apple-darwin, x86_64-unknown-linux-gnu and x86_64-pc-windows-msvc,
//     and FAILS (rc 1 => UNSIGNED) for aarch64-linux-gnu.
//   * clang on THIS file emits a SIGN-extending byte load for native
//     arm64-darwin (ldrsb x1, ldrb x0) and a ZERO-extending one under
//     --target=aarch64-linux-gnu (ldrsb x0, ldrb x1).
//   * this file's exit expression ((char)0x80 < 0 ? 10 : 20) constant-evaluates
//     to 10 for arm64-apple-darwin, x86_64-unknown-linux-gnu and
//     x86_64-pc-windows-msvc, and to 20 for aarch64-linux-gnu.
//   * THIS file, clang-built and run natively -> exit 10 at -O0 and 10 at -O2.
//   * THIS file, DSS-built for arm64:macho64-arm64-darwin-exec and run -> exit
//     10 in the baseline arm and 10 under the `release` arm. DSS and clang now
//     agree on this host.
// Only the arm64:macho64 leg actually EXECUTES on a darwin host; the other three
// are compiled and then skipped by `runOn`, and their values rest on the clang
// facts above.
//
// The `volatile` seed keeps the byte a RUNTIME value, so the optimizer cannot
// fold `(char)0x80 < 0` at compile time — the release arm exercises the real
// machine char->int promotion, not a const-fold. Per-target exitCode (see
// expected.json): 10 on the signed-`char` targets (x86_64 elf64/pe64 AND arm64
// macho64), 20 on the one unsigned-`char` target DSS ships (arm64 elf64).

int main(void) {
    volatile int seed = 0x80;   // runtime source — not const-foldable
    char c = (char)seed;        // truncate to a bare `char` holding the byte 0x80
    return c < 0 ? 10 : 20;     // char->int promotion: SExt (signed) vs ZExt (unsigned)
}
