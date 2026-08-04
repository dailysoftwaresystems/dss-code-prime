/*
 * TF-C112 (D-FFI-PE-CRT-UCRT-MIGRATION) — RUNTIME witness that re-declaring a
 * shipped symbol the platform realizes as a COMPILER-SYNTHESIZED SHIM still
 * produces a binary that LOADS and runs.
 *
 * WHY THIS EXAMPLE EXISTS, AND WHY NOTHING SHORTER PROVES IT.
 *
 * A shipped descriptor row declares two INDEPENDENT things: where the symbol
 * resolves (`library`) and whether it is an import AT ALL (`synthesize`).
 * Goal-2 gives a user re-declaration authority over the SIGNATURE — it never
 * gave it authority over the platform's REALIZATION — but the bare-prototype
 * extern synthesis used to forward only the library and then emit a plain
 * import regardless. On a `synthesize` row that is a hard load failure:
 *
 *   MEASURED against the real DLL (objdump -p C:\Windows\System32\ucrtbase.dll):
 *     ucrtbase.dll exports NO bare printf / fprintf / sprintf / vfprintf /
 *     sscanf. Only the `__stdio_common_v*` cores those five names are SHIMMED
 *     over. (msvcrt.dll, the pre-flip binding, exports all five — which is
 *     exactly why this channel sat open and inert for so long.)
 *
 *   DSS eagerly imports every declared shipped extern
 *   (D-FFI-DESCRIPTOR-EAGER-IMPORT), so naming one of those five as an import
 *   is NOT a build error. MEASURED at the TF-C111 HEAD on the three lines
 *   `#include <stdio.h>` + `int printf(const char *, ...);` + one call: the
 *   compile returned rc=0 with no diagnostic at any stage, `objdump -p` showed
 *   `printf` in the ucrtbase import table beside its four correctly-shimmed
 *   siblings, and the binary died at PROCESS START with 0xC0000139.
 *
 * ⇒ Only a RUNNABLE artifact can witness this. A compile-only test sees a clean
 *   rc=0 in both the broken and the fixed world; the difference appears the
 *   instant the loader is asked to resolve the import table. That is the whole
 *   argument for this file existing rather than one more unit pin.
 *
 * WHAT IS RE-DECLARED, AND WHY ALL FIVE. Each of the five pe stdio recipes is a
 * SEPARATE synth arm over a DIFFERENT UCRT core, and each therefore has its own
 * way to be wrong:
 *   printf   -> __stdio_common_vfprintf via the __acrt_iob_func(1) accessor;
 *   fprintf  -> the same core with a CALLER-supplied FILE *;
 *   vfprintf -> the same core again, but the ONE arm that is NOT variadic —
 *               `ap` is a declared parameter forwarded verbatim;
 *   sprintf  -> __stdio_common_vsprintf with the unbounded (size_t)-1 count;
 *   sscanf   -> __stdio_common_vsscanf.
 * Re-declaring only `printf` would leave four arms unwitnessed on this channel.
 * Every one of these prototypes is what <stdio.h> itself declares, so all five
 * are legal C that clang and GCC accept without a murmur — the point is that
 * DSS must accept them too AND keep the platform's realization.
 *
 * NOT A DUPLICATE of shipped_printf_ucrt. That example exercises the INJECTED
 * path (include the header, call the function) and pins the UCRT-vs-msvcrt
 * formatting divergence. This one exercises the SUPPRESSED path — the header is
 * included AND the names are re-declared, so the descriptor's own rows are
 * deleted and the user's prototypes are the only surviving declarations. Same
 * family, different channel; the sibling stayed green throughout the whole
 * period this defect was live.
 *
 * NOT A DUPLICATE of bare_proto_shipped_redecl either: that pins the LIBRARY
 * riding a suppressed row (`puts`, a real ucrtbase export). Its symbol has no
 * recipe, so it can never reach the arm under test here — which is itself worth
 * having, since it keeps the two paths honest about staying separate.
 *
 * CROSS-TARGET: on elf/macho the very same source takes the ORDINARY suppressed
 * import path (glibc and libSystem export real printf/fprintf/... — those rows
 * carry no `synthesize` tag), so the identical bytes must come out. That makes
 * this a per-format agreement check as well as a pe load check, and it is what
 * pins the fix as descriptor-data-driven rather than format-keyed.
 *
 * ANTI-FOLD: every formatted operand is a MUTABLE GLOBAL rather than a literal
 * argument (the shipped_printf_ucrt convention), so the `release` arm cannot
 * collapse the calls into precomputed text and leave a witness that witnesses
 * nothing.
 *
 * RETURN COUNTS ARE PLATFORM-INVARIANT, and that is measured rather than hoped
 * for: Windows text-mode streams translate \n to \r\n, but the Microsoft CRT
 * returns the count BEFORE translation (probed directly in the sibling example
 * — a 27-byte expansion with one \n returns 27 while the file on disk is 28).
 * So one set of constants is correct on all four targets and the CRLF
 * divergence is carried entirely by the manifest's per-target `expectedStdout`,
 * the shipped_atexit convention.
 *
 * ★ THE CONSTANTS ARE NOT SELF-CERTIFIED. This exact source was compiled and
 * run by REAL gcc 14.2.0 at -O2 (mingw-w64, linking msvcrt): identical stdout,
 * exit 42. So every `n != <count>` here is confirmed by a different compiler
 * against a different CRT, and the five re-declarations are confirmed legal C
 * by a toolchain that rejects an incompatible one (`gcc -fsyntax-only` on
 * `int printf(const char *);` over the same header: "error: conflicting types
 * for 'printf'"). Nothing in this file is precision-sensitive, so the msvcrt
 * reference is a valid oracle here — the UCRT-vs-msvcrt formatting divergence
 * is the sibling shipped_printf_ucrt's job, not this one's.
 *
 * exit 42 on success; a distinct code per check (50..59).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ── THE RE-DECLARATIONS. Byte-for-byte what <stdio.h> declares, which is the
 *    whole point: goal-2 deletes the descriptor's own rows and these become the
 *    sole declarations carrying every call below. ─────────────────────────── */
int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int sscanf(const char *buf, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, va_list ap);

/* The formatted operands: mutable globals, never literal arguments. */
int  g_pos     = 12345;
char g_str[]   = "redecl";
char g_scan[]  = "-77 zeta";

/* shell.c's `cli_printf` shape — a real caller-produced va_list forwarded to a
 * RE-DECLARED vfprintf. This is the only arm whose `ap` comes from a genuine
 * variadic prologue rather than from a compiler-synthesized shim's own leaf. */
static int emit(FILE *out, const char *fmt, ...) {
    va_list ap;
    int     n;
    va_start(ap, fmt);
    n = vfprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

int main(void) {
    char b[32];
    char sw[8];
    int  si = 0;
    int  n;

    /* ── 1. printf through the re-declared prototype ───────────────────────── */
    n = printf("r1 %d|%s\n", g_pos, g_str);
    if (n != 16) return 50;

    /* ── 2. fprintf to an explicit stdout — the caller-supplied FILE * arm ─── */
    n = fprintf(stdout, "r2 %d/%s\n", g_pos, g_str);
    if (n != 16) return 51;

    /* ── 3. vfprintf through a real va_list ────────────────────────────────── */
    n = emit(stdout, "r3 %s %d\n", g_str, g_pos);
    if (n != 16) return 52;

    /* ── 4. sprintf — the BUFFER core, asserted on its bytes as well as its
     *      return value, then echoed so the stdout pin covers it too. ─────── */
    n = sprintf(b, "r4 %d-%s", g_pos, g_str);
    if (n != 15) return 53;
    if (strcmp(b, "r4 12345-redecl") != 0) return 54;
    n = printf("%s\n", b);
    if (n != 16) return 55;

    /* ── 5. sscanf — the SCAN core, off a runtime buffer ───────────────────── */
    n = sscanf(g_scan, "%d %s", &si, sw);
    if (n != 2) return 56;
    if (si != -77) return 57;
    if (strcmp(sw, "zeta") != 0) return 58;
    n = printf("r5 %d %s\n", si, sw);
    if (n != 12) return 59;

    return 42;
}
