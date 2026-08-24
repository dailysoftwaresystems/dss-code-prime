/*
 * Runtime witness for the pe64 STREAM printf family — `printf`, `fprintf`
 * (stdout AND stderr), `vfprintf` and `sscanf` (D-FFI-PE-CRT-UCRT-MIGRATION,
 * Phase 3). The sibling of `shipped_sprintf_ucrt` / `..._crosscu`, which cover
 * only the BUFFER member of the family (`sprintf`).
 *
 * WHY A SECOND WITNESS IS NEEDED AT ALL. `sprintf` routes through UCRT's
 * `__stdio_common_vsprintf` and touches nothing but the CALLER's buffer. Every
 * entry point exercised HERE routes through `__stdio_common_vfprintf` /
 * `__stdio_common_vsscanf` instead, and those take a `FILE *` — a CRT-OWNED
 * handle. That drags in a whole second dependency the sprintf witnesses cannot
 * reach:
 *
 *   MEASURED against the real DLLs on this machine (objdump -p + an explicit
 *   LoadLibrary/GetProcAddress probe of C:\Windows\System32\*.dll):
 *     msvcrt.dll   exports `__iob_func`      ; ucrtbase.dll does NOT.
 *     ucrtbase.dll exports `__acrt_iob_func` ; msvcrt.dll   does NOT.
 *     ucrtbase.dll exports NO bare printf/fprintf/sprintf/vfprintf/sscanf at
 *     all — only the `__stdio_common_v*` cores.
 *
 *   `stdout`/`stderr` are pe MACROS over that accessor, so their binding MUST
 *   move in lockstep with the rest of stdio.json — and the two failure modes
 *   sit on opposite ends of the loudness scale:
 *
 *     * naming an accessor the runtime does not export is LOUD but late. DSS
 *       eagerly imports every declared symbol (D-FFI-DESCRIPTOR-EAGER-IMPORT),
 *       so it is not a build error at all — the LOADER rejects the binary with
 *       0xC0000139. A compile-only test cannot see it; this example dies at
 *       startup instead.
 *     * a WRONG INDEX is silent. `stdout` and `stderr` differ only by the 1 vs
 *       2 passed to the accessor, and a program that writes the right text to
 *       the wrong stream still produces plausible output. Only asserting the
 *       merged byte sequence with each stream carrying DISTINGUISHABLE text
 *       (the `p*`/`f*`/`v*` lines vs the `E*` lines) pins that.
 *
 *   Worth recording what is NOT a hazard here, because the earlier shape had it
 *   and the current one does not: the pe macro is a CALL, `(__acrt_iob_func(N))`,
 *   over an OPAQUE `struct FILE {}` — not `(&__iob_func()[N])`, which was
 *   pointer arithmetic over `sizeof(FILE)` and would have silently mis-addressed
 *   the stream from a stale record size. The accessor-call form removes that
 *   whole class; DSS never needs to know the record's size.
 *
 * ★ THE UCRT PROOF: `%.25e`. Two independent divergences ride in one string,
 * both MEASURED here, not assumed (probe: msvcrt!sprintf vs
 * ucrtbase!__stdio_common_vsprintf, same format, same double):
 *
 *     legacy msvcrt : 3.3333333333333331000000000e-001
 *     UCRT          : 3.3333333333333331482961626e-01
 *
 *   (a) PRECISION. msvcrt stops generating digits after ~17 SIGNIFICANT ones
 *       and zero-fills the rest; UCRT emits the exact decimal expansion of the
 *       binary double, as glibc and Darwin libc do. This is the mechanism
 *       behind the long-standing sqlite `fpconv1-2.0` failure.
 *   (b) EXPONENT WIDTH. msvcrt renders three digits (`e-001`), UCRT the
 *       C-conformant two (`e-01`).
 *
 *   So this single line fails loudly if the pe stream family ever regresses to
 *   the legacy CRT, and — because all three modern libcs expand exactly — it is
 *   simultaneously the cross-target agreement check: every hosted target must
 *   print the IDENTICAL string.
 *
 * ANTI-FOLD, two layers. Every formatted operand lives in a MUTABLE GLOBAL
 * rather than a literal argument (the `shipped_sprintf_ucrt` convention), and
 * the `%e` operand additionally comes from a RUNTIME DIVISION of two `volatile`
 * doubles — a value no constant-folder may precompute even with whole-program
 * visibility. Without that the `release` arm could fold the formatting away and
 * leave a witness that witnesses nothing.
 *
 * STREAM ORDERING IS PINNED, NOT ASSUMED. The examples harness merges the
 * child's stdout AND stderr into ONE captured buffer (tests/test_support/
 * run_binary.hpp: `si.hStdOutput = si.hStdError = pipeWrite` on Windows, two
 * `dup2`s onto fds 1 and 2 on POSIX). Piped stdout is fully buffered while
 * stderr is not, so the interleaving would otherwise be an implementation
 * detail. Every stderr write here is therefore preceded by an explicit
 * `fflush(stdout)` and followed by `fflush(stderr)`, which makes the merged
 * byte sequence deterministic on all four targets.
 *
 * THE RETURN COUNTS ARE PLATFORM-INVARIANT, and that is MEASURED rather than
 * hoped for. Windows text-mode streams translate `\n` to `\r\n`, so the pe
 * stdout pin differs from the elf/macho one — but the Microsoft CRT returns the
 * count BEFORE that translation (probed directly against msvcrt.dll: a format
 * expanding to 27 bytes with one `\n` returns 27 while the file on disk is 28;
 * `"a\nb\n"` returns 4 against 6 bytes on disk). So one set of `n != <count>`
 * constants is correct on every target, and the CRLF divergence is carried
 * entirely by the manifest's per-target `expectedStdout` — the shipped_atexit
 * convention.
 *
 * exit 42 on success; a distinct code per check (50..73).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ── the formatted operands: mutable globals, never literal arguments ─────── */
int          g_pos    = 12345;                  /* %d  — a positive int */
int          g_neg    = -42;                    /* %d  — a negative int */
char         g_ch     = 'Q';                    /* %c  — a char */
char         g_str[]  = "printf";               /* %s  — a string */
unsigned int g_hex    = 0xDEADBEEFu;            /* %x  — unsigned hex */
long long    g_big    = 9223372036854775807LL;  /* %lld — INT64_MAX */
int          g_zero8  = 42;                     /* %08d — zero-padded width */
int          g_plus   = 7;                      /* %+d  — the '+' flag */
double       g_half   = 3.5;                    /* %.2f — a float; no rounding tie */

/* THE %e OPERAND. `volatile` so the division below is a genuine RUNTIME
 * computation: 1.0/3.0 is the shortest expression whose exact binary value
 * needs far more than msvcrt's ~17 significant digits to print, which is
 * exactly what makes the legacy CRT distinguishable from UCRT/glibc/Darwin. */
volatile double g_v_one   = 1.0;
volatile double g_v_three = 3.0;

/* THE sscanf INPUT: one runtime buffer feeding four conversions. */
char g_scan[] = "-4242 beta 7b 2.5";

/* shell.c's `cli_printf` shape — a real va_list forwarded to vfprintf. This is
 * sqlite's load-bearing output path, and the ONLY member of the family whose
 * `ap` is produced by the CALLER's variadic prologue rather than by a
 * compiler-synthesized shim. */
static int emit(FILE *out, const char *zFormat, ...) {
    va_list ap;
    int     n;
    va_start(ap, zFormat);
    n = vfprintf(out, zFormat, ap);
    va_end(ap);
    return n;
}

int main(void) {
    double third;
    int    n;
    int    si = 0;
    unsigned int sx = 0u;
    double sd = 0.0;
    char   sw[16];

    /* stdout and stderr must be DISTINCT streams. A wrong iob index or a wrong
     * FILE stride can collapse them onto the same record, and every text check
     * below would still pass because the harness merges the two pipes. */
    if (stdout == stderr) return 50;

    /* ── 1. printf: %d, %s, %c, %x ─────────────────────────────────────────── */
    n = printf("p1 %d|%s|%c|%x\n", g_pos, g_str, g_ch, g_hex);
    if (n != 27) return 51;   /* the real byte count, not garbage */

    /* ── 2. printf: %lld past INT32_MAX, %08d, %+d, and a float ────────────── */
    n = printf("p2 %lld|%08d|%+d|%.2f\n", g_big, g_zero8, g_plus, g_half);
    if (n != 40) return 52;

    /* ── 3. fprintf to stdout — the explicit-stream sibling of printf. On pe
     *      this is the check that the `stdout` macro resolves to a REAL FILE
     *      record through the CRT's iob accessor. ─────────────────────────── */
    n = fprintf(stdout, "f1 %d/%s/%c/%x/%d\n", g_pos, g_str, g_ch, g_hex, g_neg);
    if (n != 31) return 53;

    /* ── 4. vfprintf to stdout via a real va_list ──────────────────────────── */
    n = emit(stdout, "v1 %d %.2f %s\n", g_neg, g_half, g_str);
    if (n != 19) return 54;

    /* ── 5. ★ THE UCRT PROOF. A runtime 1.0/3.0 at 25 digits of precision.
     *      Legacy msvcrt prints 3.3333333333333331000000000e-001 (17
     *      significant digits, zero-filled, three-digit exponent); UCRT, glibc
     *      and Darwin libc all print the exact expansion with a two-digit
     *      exponent. ────────────────────────────────────────────────────────── */
    third = g_v_one / g_v_three;
    n = printf("e1 %.25e\n", third);
    if (n != 35) return 55;

    /* ── 6. sscanf: four conversions off one runtime buffer, then printed back
     *      so the parse is asserted through the BYTE-EXACT stdout pin as well
     *      as through the individual comparisons. ──────────────────────────── */
    n = sscanf(g_scan, "%d %s %x %lf", &si, sw, &sx, &sd);
    if (n != 4) return 56;
    if (si != -4242) return 57;
    if (strcmp(sw, "beta") != 0) return 58;
    if (sx != 0x7bu) return 59;
    if (sd != 2.5) return 60;
    n = printf("s1 %d %d %s %x %.2f\n", n, si, sw, sx, sd);
    if (n != 24) return 61;

    /* ── 7. THE STDERR ARM. Flush stdout first so the merged capture order is
     *      deterministic rather than a buffering artifact. ─────────────────── */
    if (fflush(stdout) != 0) return 62;

    n = fprintf(stderr, "E1 %d|%d|%.2f|%s\n", g_pos, g_neg, g_half, g_str);
    if (n != 25) return 63;

    /* vfprintf to stderr — the second stream through the va_list path. */
    n = emit(stderr, "E2 %s|%x|%+d\n", g_str, sx, g_plus);
    if (n != 16) return 64;

    if (fflush(stderr) != 0) return 65;

    /* ── 8. The operands survived every call unmutated (a shim that clobbered a
     *      caller-saved register or over-wrote the home area would corrupt one
     *      of these rather than print wrong text). ─────────────────────────── */
    if (g_pos != 12345) return 66;
    if (g_neg != -42) return 67;
    if (g_ch != 'Q') return 68;
    if (strcmp(g_str, "printf") != 0) return 69;
    if (g_hex != 0xDEADBEEFu) return 70;
    if (g_big != 9223372036854775807LL) return 71;
    if (g_half != 3.5) return 72;
    if (strcmp(g_scan, "-4242 beta 7b 2.5") != 0) return 73;

    return 42;
}
