// c155 D-LK10-CRT-INIT-INVOKE closure witness #3 (2026-07-17):
// setlocale — the anchor's third cited "genuinely CRT-init-requiring"
// case (locale state initialized by the CRT before main).
//
// The c155 diagnosis DISPROVED the premise: setlocale works on every
// runnable leg TODAY with the trampoline calling main directly — the
// locale subsystem is initialized to the "C" locale by libc's OWN
// loader-run initialization (pe: the C runtime DLL's own attach-time
// init — ucrtbase.dll since the UCRT-P4 flip, msvcrt.dll before it,
// and the observed result is the same under both; elf: ld.so runs
// libc.so.6's initializers; macho: dyld runs libSystem's before
// LC_MAIN). See printf_float/main.c for the per-format contract.
//
// The prototype stays an inline extern (the printf_int precedent —
// this witness includes no header), but WHAT BINDS IT CHANGED in
// UCRT-P4: c-subset.lang.json's per-name `externLibraryByFormat`
// guess was retired, so a bare extern no longer inherits "the
// format's default runtime library". The shipped-descriptor corpus
// is now the single owner of realization, and this name binds
// through stdlib.json's `setlocale` row + that descriptor's own
// `library` map (pe ucrtbase.dll / elf libc.so.6 / macho
// /usr/lib/libSystem.B.dylib). Delete the row and every leg here is
// K_SymbolUndefined — measured, not assumed.
//
// Category 0 is deliberate: the pe CRT spells LC_ALL=0 while glibc
// spells LC_CTYPE=0 — BOTH are valid categories whose startup value
// is the "C" locale, so setlocale(0, "C") returns the string "C" on
// every leg (pe echoes LC_ALL="C"; glibc echoes LC_CTYPE="C"). The
// UCRT flip does not disturb this: LC_ALL is 0 in the UCRT header
// too (measured, Windows Kits ucrt/locale.h), so the literal 0 keeps
// meaning the same category before and after.
// A shipped <locale.h> would carry the per-format LC_* constants
// (the errno.json E*-arms precedent) — out of scope here; this
// witness pins the CRT-state question, not the header surface.
//
// Failure modes are split: NULL return (locale subsystem dead)
// exits 1; a non-"C" string flips the byte-exact stdout pin; a
// crash inside setlocale/printf flips the exit code.

extern char* setlocale(int category, const char* locale);
extern int printf(const char* fmt, ...);

int main(void) {
    char* p = setlocale(0, "C");
    if (p == 0) return 1;
    printf("%s\n", p);
    return 42;
}
