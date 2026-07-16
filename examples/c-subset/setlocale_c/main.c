// c155 D-LK10-CRT-INIT-INVOKE closure witness #3 (2026-07-17):
// setlocale — the anchor's third cited "genuinely CRT-init-requiring"
// case (locale state initialized by the CRT before main).
//
// The c155 diagnosis DISPROVED the premise: setlocale works on every
// runnable leg TODAY with the trampoline calling main directly — the
// locale subsystem is initialized to the "C" locale by libc's OWN
// loader-run initialization (pe: msvcrt DllMain at attach; elf:
// ld.so runs libc.so.6's initializers; macho: dyld runs libSystem's
// before LC_MAIN). See printf_float/main.c for the per-format
// contract.
//
// setlocale ships in no descriptor (there is no shipped <locale.h>
// yet), so the prototype is an inline extern — the printf_int
// precedent; the linker binds it against the format's default
// runtime library (msvcrt.dll / libc.so.6 / libSystem.B.dylib).
//
// Category 0 is deliberate: msvcrt spells LC_ALL=0 while glibc
// spells LC_CTYPE=0 — BOTH are valid categories whose startup value
// is the "C" locale, so setlocale(0, "C") returns the string "C" on
// every leg (msvcrt echoes LC_ALL="C"; glibc echoes LC_CTYPE="C").
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
