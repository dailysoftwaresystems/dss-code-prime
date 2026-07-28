// D-CSUBSET-GNU-ATTRIBUTE (TF-C62) witness: GNU `__attribute__((...))` in the
// AFTER-DECLARATOR position, with the widened argument grammar. Before TF-C62 the
// after-declarator position failed loud (`P0009 got '__attribute__'`) and the
// argument grammar accepted only `name` / `name("string")` — so `alloc_size(8)`
// (number), `format(printf,1,2)` (multi-arg), `__nonnull__((1))` (nested), and a
// multi-clause `a, b` run all failed. This is the shape every real C header uses
// on its prototypes — glibc: `extern int remove(const char*) __attribute__((
// __nothrow__, __leaf__));` ; Tcl: `Tcl_Panic(const char*,...) TCL_FORMAT_PRINTF(
// 1,2);`. The attributes are parse-and-ignore hints here (they do not change the
// program's behavior), so the point is that the program COMPILES and RUNS with
// them present, exactly as C requires.
//
// ★★ EVERY ATTRIBUTE BELOW IS WRITTEN IN A SHAPE A REAL COMPILER ACCEPTS, and
// that is a hard requirement, not a nicety: this file's job is to witness that
// DSS handles REAL C. An attribute clang rejects or ignores would make the
// example prove the opposite — that DSS accepts something no toolchain does —
// and would make it a worthless red-on-disable witness besides, since the
// attribute under test would be one clang says does not apply. VERIFIED with
// `clang -fsyntax-only -Wall -Wextra -isysroot $(xcrun --show-sdk-path)`:
// ZERO errors, ZERO warnings; and the clang-built binary also exits 42.
// Re-run that check if you touch this file.
//
// Covers, all in the after-declarator position:
//   * multi-arg       `__attribute__((format(printf,1,2)))`  on a real format fn
//   * multi-clause    `__attribute__((__nothrow__, __leaf__))`
//   * nested-paren    `__attribute__((__nonnull__((1))))`
//   * number arg      `__attribute__((alloc_size(1)))`   on a POINTER-returning fn
//   * bare on object  `__attribute__((__unused__))`      on an object declarator
//
// ★ TF-C73 REWORKED THE ATTRIBUTE SET HERE, for two independent reasons.
//
// (1) This file used to carry `int v __attribute__((aligned(4))) = 20;` as its
//     number-arg witness, and it only compiled because the after-declarator
//     position was parse-and-IGNORE — i.e. the alignment was being SILENTLY
//     DROPPED. `aligned` is ABI-affecting and DSS sourced alignment only from
//     `alignasSpec`, so it failed loud in the LEADING position; TF-C73 made the
//     trailing position agree (D-CSUBSET-GNU-ATTRIBUTE's pinned after-declarator
//     follow-up — one attribute must not mean two different things depending on
//     which side of the declarator it sits). Do NOT restore `aligned` here: it
//     would witness nothing this file's other rows do not already cover, and a
//     compiles-clean row is exactly the witness that cannot detect a dropped
//     alignment.
//
//     ★ THE PINNED FOLLOW-UP IS DISCHARGED — this note used to end "it comes
//     back when the real `aligned` sink lands, asserting the ALIGNMENT, not
//     merely the parse". The sink LANDED in TF-C73, and the case came back in
//     `examples/c-subset/gnu_aligned_attribute/` rather than here, because that
//     example asserts something STRICTLY STRONGER: it gates its exit code on the
//     RUNTIME ADDRESS (and, for the member, on `sizeof`) in every position that
//     can carry the attribute — leading, after-declarator, typedef, and struct
//     member — so a dropped or wrong alignment fails the run rather than the
//     build.
//     Restoring `aligned(4)` here would only have re-proved that the declaration
//     COMPILES — and `aligned(4)` on an `int` is not even an over-alignment, so
//     it could not have failed at all. This file's job is the ARGUMENT GRAMMAR
//     (multi-arg / multi-clause / nested / number) on parse-and-ignore hints;
//     alignment HONORING is the other example's job. Keep the split.
//
// (2) The `format` witness was INVALID C and had been since TF-C62: it read
//     `int firstv(int n, ...) __attribute__((format(printf, 1, 2)))`, and clang
//     rejects that outright — `error: format argument not a string type`,
//     because argument 1 is an `int`, not a format string. It is now written on
//     a genuine format function (`const char *fmt` at position 1, first variadic
//     at 2). The number-arg role moved to `alloc_size(1)` on a POINTER-returning
//     allocator, which is where that attribute legally belongs
//     (`-Wignored-attributes` fires on a non-pointer return).
//
// The arithmetic (each call accumulates) yields 42; the attributes must not alter
// it. RED-ON-DISABLE: revert the after-declarator `attrSpec` slot in
// c-subset.lang.json (initDeclarator) → the first prototype fails P0009 and the
// program no longer compiles (the runner reports a compile failure, not exit 42).
//
// Front-end feature (grammar + declaration lowering), target/format-agnostic:
// x86_64 (PE + ELF) and arm64 (ELF under qemu, Mach-O macos leg).

// A locally-DEFINED variadic carrying a multi-arg `format` attribute (no extern,
// so nothing to link). Position 1 IS the format string and position 2 IS the
// first variadic argument, so the attribute is valid — clang even type-checks
// the `logfmt("%d", 7)` call site below against it. Returns 0 for "%d".
int logfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int logfmt(const char *fmt, ...) { return fmt[0] - '%'; }

int add(int a, int b) __attribute__((__nothrow__, __leaf__));
int add(int a, int b) { return a + b; }

int deref(const int *p) __attribute__((__nonnull__((1))));
int deref(const int *p) { return *p; }

// The NUMBER-ARG witness: `alloc_size(1)` says "the returned block is `n` bytes",
// which is only meaningful — and only accepted without -Wignored-attributes — on
// a function RETURNING A POINTER. A bump-allocator stub is the smallest honest
// shape that satisfies that and still uses its parameter.
static char pool[64];
void *raw(unsigned long n) __attribute__((alloc_size(1)));
void *raw(unsigned long n) { return n <= 64 ? (void *)pool : (void *)0; }

int v __attribute__((__unused__)) = 20;

int main(void) {
    int twelve = 12;
    int s = add(v, twelve);              // 20 + 12 = 32   (v is the __unused__ global)
    int ten = 10;
    s = s + deref(&ten);                 // 32 + 10 = 42
    s = s + logfmt("%d", 7);             // + 0 = 42        (format-attributed variadic)
    s = s + (raw(8) == (void *)0 ? 1 : 0);   // + 0 = 42    (alloc_size number-arg)
    return s;                            // 42
}
