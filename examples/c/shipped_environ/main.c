/* POSIX `environ` — the SMOKE TEST: does reading the environment through the
   shipped `environ` -> `__environ` macro/symbol split actually WORK in a
   DSS-built ELF exec, on both arches, debug and release.

   ★★ READ THIS BEFORE TRUSTING WHAT THIS EXAMPLE PROVES, BECAUSE IT ONCE
   PROVED LESS THAN IT CLAIMED. This program is a SELF-LOOP and cannot witness
   an OBJECT-IDENTITY property: it reads `environ` through the DSS macro and
   cross-checks the value against libc's own `getenv`, but under the ELF COPY
   RELOCATION binding that shipped at the time, BOTH of those reads went through
   the very name the exec CLAIMED, so it compared the exec's copy WITH ITSELF.
   It therefore stayed GREEN on all four legs across the whole window in which a
   third-party `libtcl8.6.so` reading the UN-PREFIXED `environ` got NULL and
   SIGSEGV'd in `TclSetupEnv+0xe8` (`09e1608a` green -> `6f4aab73` rc=139).
   THE GENERAL RULE that came out of it, now recorded in the anchor: AN
   OBJECT-IDENTITY PROPERTY CANNOT BE WITNESSED BY ONE IMAGE — a test asserting
   that a symbol denotes the SAME OBJECT must involve a SECOND image DSS did not
   build. That witness is `examples/c/environ_alias_object_identity`,
   which loads a gcc-built `.so` reading the un-prefixed name and asserts the
   VALUE it sees, plus `dlsym` SAME_OBJECT=YES on both spellings.
   ⇒ This example is kept for what it DOES witness — a live, well-formed
   environment vector reachable through the shipped descriptor on both ELF
   arches — and NOT for object identity.
   (D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET.)

   WHY `environ` AND NOT main's third parameter: POSIX.1 does not specify a
   third parameter to main AT ALL. `environ` is the blessed, portable spelling,
   so it is what real code reaches for.

   WHY THE SYMBOL IS `__environ`: glibc declares `__environ`
   UNCONDITIONALLY (/usr/include/unistd.h) and exports it STRONG, while
   `environ` sits behind `#ifdef __USE_GNU` and is exported WEAK. DSS
   EAGER-IMPORTS every name a descriptor declares, so the strong,
   always-present spelling is the safe one — and the POSIX spelling reaches it
   through an elf-gated macro. ⓘ It is no longer a CORRECTNESS requirement:
   under the got-indirect binding this example now uses, ld.so resolves either
   spelling to glibc's ONE object. It WAS one under copy relocation, and that is
   history worth keeping rather than a live claim.

   Binding model: every ELF format declares `dataImportBinding: "got-indirect"`
   — the exec reserves a GOT slot, emits one R_*_GLOB_DAT against it, ld.so
   writes the LIBRARY object's address in, and the shared GotIndirect lowering
   derefs it. The exec DEFINES NOTHING.
   EAGER-IMPORT SAFE, measured before the row shipped: glibc exports all three
   spellings at ONE address on BOTH run legs — `__environ` STRONG (`B`/`g`),
   `environ`/`_environ` WEAK (`V`/`w`) — 0x20ad58 @GLIBC_2.2.5 (x86_64) /
   0x1b7288 @GLIBC_2.17 (aarch64), `nm -D` and `objdump -T` concurring.
   ⚠ the `@@GLIBC_x.y` suffix `readelf` prints makes a grep anchored on `$`
   report a FALSE ABSENT.

   The assertion ladder never asserts the environment's CONTENT (it is
   host-dependent), only its STRUCTURE plus an agreement check:
   - `environ == 0` -> exit 1: nothing bound the object at all.
   - every entry must be `NAME=VALUE` with a NON-EMPTY name -> exit 2 / 3. Bound
     one indirection off (the slot's ADDRESS read as the vector — the
     got-indirect-without-deref error), the walk sees garbage pointers and the
     '=' scan fails here rather than silently "passing".
   - the vector must hold at least one entry -> exit 4.
   - the AGREEMENT CHECK: take environ[0]'s NAME, look it up with libc's OWN
     `getenv`, and require the returned VALUE to be byte-identical to the text
     after environ[0]'s '=' (exit 7 = getenv cannot see a variable environ
     claims exists; exit 8 = the two disagree). ⚠ This is the SELF-LOOP half:
     `getenv` reads libc's internal `__environ`, i.e. the same object this
     program reads, so it cannot detect a SPLIT — only a garbage or stale
     pointer.

   RED-ON-DISABLE: un-ship the row -> honest S0001; un-ship the macro -> honest
   S0001 `got environ`; drop `dataImportBinding` from the elf exec format -> the
   linker's loud data-import reject (K_FormatLacksImportSupport); neuter the
   got-indirect deref (a bare lea) -> the vector walk reads the slot's own
   address as `environ[0]` and fails the '=' scan at exit 2.
   ⓘ NOT red-on-disable here any more, and that is the whole point of the sibling
   example: repointing the row at the WEAK `environ` spelling now WORKS (ld.so
   resolves it to the same object), where under copy relocation it returned 1.
   elf-ONLY on purpose: no Windows CRT exports a spelling ucrtbase can bind
   (measured — ucrtbase has none at all, msvcrt only the underscored
   `_environ`), and the macho export could not be measured this cycle, so both
   stay fail-loud S0001 per this directory's need-driven staging rule.
   Cross-checked in WSL against `gcc -std=gnu17 -no-pie` (exit 42, identical
   stdout) — `-std=gnu17`, not `-std=c17`, because glibc gates the `environ`
   spelling behind __USE_GNU. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    char **p;
    char *entry;
    char *eq;
    char *scan;
    char *val;
    char *got;
    char name[256];
    long n;
    long i;
    long nameLen;

    if (environ == 0) return 1;              /* slot never filled by the loader */

    /* STRUCTURE: every entry is NAME=VALUE with a non-empty NAME. */
    n = 0;
    for (p = environ; *p != 0; ++p) {
        entry = *p;
        eq = 0;
        for (scan = entry; *scan != 0; ++scan) {
            if (*scan == '=') { eq = scan; break; }
        }
        if (eq == 0) return 2;               /* no '=' — not an environment vector */
        if (eq == entry) return 3;           /* empty NAME */
        ++n;
    }
    if (n < 1) return 4;                     /* an empty vector is implausible */

    /* AGREEMENT: libc's own getenv must see environ[0]'s variable, same value. */
    entry = environ[0];
    eq = 0;
    for (scan = entry; *scan != 0; ++scan) {
        if (*scan == '=') { eq = scan; break; }
    }
    if (eq == 0) return 5;
    nameLen = eq - entry;
    if (nameLen > 255) return 6;             /* name too long for the buffer */
    for (i = 0; i < nameLen; ++i) name[i] = entry[i];
    name[nameLen] = 0;
    got = getenv(name);
    if (got == 0) return 7;                  /* getenv cannot see it: not live */
    val = eq + 1;
    for (i = 0; got[i] != 0 || val[i] != 0; ++i) {
        if (got[i] != val[i]) return 8;      /* the two disagree: stale copy */
    }

    puts("environ:live");
    if (fflush(0) != 0) return 9;
    return 42;
}
