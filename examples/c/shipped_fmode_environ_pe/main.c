/* LANE-O (D-CONFIG-NO-DESCRIPTOR-FOR-SETLOCALE-FMODE-ENVIRON verdict 2): the
   pe64 RUN witness for `_fmode` and `_environ` reached THE WAY THE SDK REACHES
   THEM — by `#include <stdlib.h>`, through the `__p__fmode` / `__p__environ`
   accessor macros, with NO `extern` declaration anywhere in this file.

   ★ WHY THERE IS NO `extern` HERE, and why that is the whole point. These names
   used to be declared by hand as `extern int _fmode;` / `extern char
   **_environ;` (and briefly with an invented form that appended an image-name
   string literal after the declarator — not spelled out here so a grep-based
   sweep cannot match this prose — which is not C at all;
   [[D-CSUBSET-EXTERN-LIBRARY-SYNTAX-IS-NOT-VALID-C]]).
   That cannot work on UCRT: ✔MEASURED 2026-08-10 on two concurring instruments
   (GNU objdump 2.42 `-p`, MSVC dumpbin 14.44.35207 `-exports`) over ALL 2,484
   exports of the live ucrtbase.dll 10.0.26100.8875, `_fmode` and `_environ` are
   BOTH ABSENT; the CRT publishes them only via `__p__fmode` (ordinal 88) and
   `__p__environ` (ordinal 87). Under [[D-FFI-DESCRIPTOR-EAGER-IMPORT]] a
   `kind: object` row for either name would break the LOAD (0xC0000139) of every
   binary that touched it, so for pe this class is ACCESSOR-OR-NOTHING. The real
   SDK agrees rather than merely permitting: ucrt/stdlib.h:255 is literally
   `#define _fmode (*__p__fmode ())` and :1171 `#define _environ
   (*__p__environ())`, both in the DEFAULT arm. stdlib.json now ships exactly
   that, pe-gated, so a consumer writes nothing but the include.

   ★★ EVERY LAYER IS AN AGREEMENT AGAINST AN INDEPENDENT ORACLE, never a magic
   value — the improvement over the predecessor witness, which pinned
   `_fmode == 0` and so asserted a CRT-version fact that read IDENTICALLY to a
   dead binding:
   - `_fmode` is cross-checked against `_get_fmode()`, UCRT's OWN documented
     getter for the same state (ordinal 320, ✔MEASURED). A macro expanding to a
     wrong accessor, or an accessor bound to the wrong export, disagrees.
   - `_fmode` is then WRITTEN through (it must be an LVALUE — this is why the
     macro is `(*__p__fmode())` and not a `_get_fmode`/`_set_fmode` pair, which
     could not be assigned through), read back through BOTH the macro and
     `_get_fmode`, and restored. A read-only or one-shot binding fails here.
   - `_environ` is validated STRUCTURALLY (every entry is NAME=VALUE with a
     non-empty NAME) and then cross-checked against `getenv`, the shipped_environ
     agreement pattern: environ[0]'s name must resolve through libc's own getenv
     to a byte-identical value. The environment's CONTENT is never asserted, so
     the pin is host-independent.

   RED-ON-DISABLE: drop stdlib.json's pe `_fmode` macro variant -> `_fmode` is an
   ordinary undeclared identifier and this TU fails to compile (honest, loud);
   repoint either macro at the wrong accessor (`__p___argv`, say) -> exit 3 or 8;
   un-ship `__p__fmode` / `__p__environ` -> honest K_SymbolUndefined at link;
   un-ship `_get_fmode` -> the ORACLE disappears and the TU fails to compile
   rather than silently losing a layer. pe64-ONLY: these are Microsoft spellings
   with no glibc or libSystem export, which is also why both the symbol rows and
   the macro variants are `availableObjectFormats: [pe]` / `when.format: pe`; the
   POSIX `environ` spelling is a DIFFERENT row (unistd.json, elf-only, bound
   got-indirect onto the strong `__environ`) witnessed by shipped_environ and,
   for object identity across an image boundary,
   environ_alias_object_identity. */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    char **env;
    char *entry;
    char *scan;
    char *eq;
    char *val;
    char *got;
    char name[256];
    long n;
    long i;
    long nameLen;
    int saved;
    int probed;

    /* ── _fmode: the accessor macro must agree with UCRT's own getter ── */
    probed = -1;
    if (_get_fmode(&probed) != 0) return 2;   /* the ORACLE itself failed */
    if (_fmode != probed) return 3;           /* AGREEMENT on the same state */

    saved = _fmode;
    _fmode = 0x4000;                          /* _O_TEXT, through the lvalue */
    if (_fmode != 0x4000) return 4;           /* the store did not land */
    probed = -1;
    if (_get_fmode(&probed) != 0) return 5;
    if (probed != 0x4000) return 6;           /* the getter cannot see it */
    _fmode = saved;
    if (_fmode != saved) return 7;            /* restored the CRT default */

    /* ── _environ: structure, then agreement with getenv ── */
    env = _environ;
    if (env == 0) return 8;                   /* accessor handed back nothing */

    n = 0;
    for (; env[n] != 0; ++n) {
        entry = env[n];
        eq = 0;
        for (scan = entry; *scan != 0; ++scan) {
            if (*scan == '=') { eq = scan; break; }
        }
        if (eq == 0) return 9;                /* not an environment vector */
        if (eq == entry) return 10;           /* empty NAME */
    }
    if (n < 1) return 11;                     /* an empty vector is implausible */

    entry = env[0];
    eq = 0;
    for (scan = entry; *scan != 0; ++scan) {
        if (*scan == '=') { eq = scan; break; }
    }
    if (eq == 0) return 12;
    nameLen = eq - entry;
    if (nameLen > 255) return 13;             /* too long for the buffer */
    for (i = 0; i < nameLen; ++i) name[i] = entry[i];
    name[nameLen] = 0;
    got = getenv(name);
    if (got == 0) return 14;                  /* getenv cannot see it: not live */
    val = eq + 1;
    for (i = 0; got[i] != 0 || val[i] != 0; ++i) {
        if (got[i] != val[i]) return 15;      /* the two disagree: stale table */
    }

    if (fputs("fmode-environ:live\n", stdout) < 0) return 16;
    if (fflush(0) != 0) return 17;
    return 42;
}
