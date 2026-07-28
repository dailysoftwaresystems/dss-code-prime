// TF-C56 (D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET): bare `char`'s signedness
// is TARGET-defined, and it is decided by the (architecture x PLATFORM) pair --
// NOT by the architecture alone. An earlier revision of this header claimed the
// AArch64 procedure-call standard MAKES bare `char` UNSIGNED, and the matching
// config comment claimed AAPCS64 and Apple arm64 AGREE. Both are false as
// stated. What is true: they agree only on the AArch64 *ABI DEFAULT*. AAPCS64
// leaves bare-`char` signedness IMPLEMENTATION-DEFINED; the AArch64/GNU-Linux
// platform ABI chose UNSIGNED, Apple's arm64 platform ABI DIVERGED and chose
// SIGNED, and clang follows the PLATFORM, not the architecture. x86_64 (SysV +
// Windows/pe64) is SIGNED everywhere. A `char` holding the byte 0x80 therefore
// promotes to `int` as -128 wherever `char` is signed (SExt / sxtb / ldrsb) and
// as +128 where it is unsigned (ZExt / uxtb / ldrb), so `c < 0` is TRUE on
// x86_64 AND on arm64-DARWIN, and FALSE on arm64-LINUX.
//
// MEASURED BY THIS AGENT 2026-07-28 on this host (Darwin 25.5.0 / arm64, Apple
// clang 21.0.0 / clang-2100.1.1.101), exit codes captured DIRECTLY, never after
// a pipe:
//   * clang -dM -E -x c /dev/null -target aarch64-linux-gnu  DEFINES
//     __CHAR_UNSIGNED__ 1;  -target arm64-apple-darwin does NOT define it.
//   * _Static_assert((char)-1 < 0) compiles clean (rc 0) for arm64-apple-darwin,
//     x86_64-unknown-linux-gnu and x86_64-pc-windows-msvc, and FAILS (rc 1,
//     note "(0xFF, 255) < 0") for aarch64-linux-gnu.
//   * clang on THIS file emits `ldrsb` (sign-extend) for native arm64-darwin and
//     `ldrb` (zero-extend) under --target=aarch64-linux-gnu.
//   * THIS file, clang-built and run natively -> exit 10 at -O0 and 10 at -O2.
//   * THIS file, DSS-built for arm64:macho64-arm64-darwin-exec and run -> exit
//     20 in debug and 20 under --config=release.
//
// !!! THE arm64:macho64 ROW IN expected.json PINS THAT 10-vs-20 DIVERGENCE AS A
// KNOWN DEFECT -- IT DOES NOT ASSERT CORRECT BEHAVIOUR. Anchor:
// D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM (OPEN, CONFIRMED). Root cause:
// src/dss-config/targets/arm64.target.json declares ONE flat
// `charIsUnsigned: true` for the whole architecture -- right for
// aarch64-linux-gnu, wrong for arm64-apple-darwin -- and that key cannot express
// the (architecture x platform) split. It is a SILENT wrong-value miscompile: no
// diagnostic, no crash, just different numbers on every negative-`char` path.
// Clang ground truth on that row is 10; the pinned DSS value is 20. WHEN THE
// ANCHOR CLOSES THAT EXPECTATION MUST FLIP 20 -> 10 -- the example must not be
// deleted, relaxed, or weakened to silence a failure here. The full measured
// record lives in that row's own $comment in expected.json; read it first.
//
// The `volatile` seed keeps the byte a RUNTIME value, so the optimizer cannot
// fold `(char)0x80 < 0` at compile time — the release arm exercises the real
// machine char->int promotion (the exact codegen this cycle fixes), not a
// const-fold. Per-target exitCode (see expected.json): 10 on the signed-char
// targets (x86_64 elf/pe64), 20 on the targets DSS resolves as unsigned-char
// (arm64 elf -- correct; arm64 macho -- the pinned defect flagged above).

int main(void) {
    volatile int seed = 0x80;   // runtime source — not const-foldable
    char c = (char)seed;        // truncate to a bare `char` holding the byte 0x80
    return c < 0 ? 10 : 20;     // char->int promotion: SExt (signed) vs ZExt (unsigned)
}
