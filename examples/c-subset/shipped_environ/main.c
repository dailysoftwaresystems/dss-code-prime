/* LANE-F (D-LK-EXTERN-DATA-IMPORT, a second elf copy-relocation consumer):
   POSIX's `environ` — the environment vector itself. It ships from unistd.json
   as the MACRO/SYMBOL SPLIT `environ` -> `__environ`: an elf-gated macro onto a
   `kind: object` DATA row typed `ptr<ptr<char>>`, availableObjectFormats:[elf].

   WHY `environ` AND NOT main's third parameter: POSIX.1 does not specify a
   third parameter to main AT ALL. `environ` is the blessed, portable spelling,
   so it is what real code reaches for — which is exactly why it has to bind
   correctly.

   ★★ WHY THE SYMBOL IS `__environ` AND NOT `environ` — the whole point of this
   witness, and NOT a stylistic choice. The obvious shape (an `environ` data
   row) WAS built first and was SILENTLY WRONG: the binary looked perfect
   (`readelf -r` showed `R_X86_64_COPY … environ + 0`, the dynsym showed
   `environ` as an 8-byte OBJECT) and `environ` READ NULL at runtime on all four
   legs — this program returned 1 from its very first guard. A matched
   `gcc -std=gnu17 -no-pie` control of the same source RUNS, and ITS copy
   relocation is against `__environ@GLIBC_2.2.5` — the STRONG alias — with both
   `__environ` (GLOBAL) and `environ` (WEAK) defined at ONE exec address.
   MECHANISM: glibc's startup writes the environment pointer through its
   INTERNAL name `__environ`, and an ELF copy relocation only redirects libc's
   own references for the symbol THE EXEC CLAIMS. Claim the weak `environ`
   alone and libc keeps writing its own .bss slot while the exec reads a copy
   ld.so filled from that slot BEFORE `__libc_start_main` ran — i.e. 0. This is
   the TF-C121 `_realpath$DARWIN_EXTSN` class: the declared name links clean,
   loads clean, and binds the wrong thing.
   ⓘ It also mirrors the real header: glibc declares `__environ`
   UNCONDITIONALLY and `environ` only under `#ifdef __USE_GNU` — which is why
   the gcc cross-check needs `-D_GNU_SOURCE` (or its own extern) and a plain
   `-std=c17` compile of `environ` fails `'environ' undeclared`.

   Binding model: the ELF exec formats declare
   `dataImportBinding: "copy-relocation"`, so the exec reserves a .bss slot,
   exports it as a DEFINED OBJECT dynsym (st_size = the layout-derived pointer
   width, never hardcoded), and ld-linux copies libc's object in before entry.
   EAGER-IMPORT SAFE, measured before the row shipped: glibc exports all three
   spellings at ONE address on BOTH run legs — `__environ` STRONG (`B`/`g`),
   `environ`/`_environ` WEAK (`V`/`w`) — 0x20ad58 @GLIBC_2.2.5 (x86_64) /
   0x1b7288 @GLIBC_2.17 (aarch64), `nm -D` and `objdump -T` concurring,
   `DO .bss` size 8.

   The assertion ladder discriminates every wrong-binding class, and the CONTENT
   of the environment is never asserted (it is host-dependent), only its
   STRUCTURE plus an agreement check:
   - `environ == 0` -> exit 1: the .bss slot was never filled (a dead slot, or
     `dataImportBinding` dropped so nothing copies).
   - every entry must be `NAME=VALUE` with a NON-EMPTY name -> exit 2 / 3. Bound
     one indirection off (the slot's ADDRESS read as the vector, the classic
     got-indirect-without-deref error), the walk sees garbage pointers and the
     '=' scan fails here rather than silently "passing".
   - the vector must hold at least one entry -> exit 4.
   - ★ THE AGREEMENT CHECK, which is what proves the copied slot is libc's LIVE
     object and not a stale duplicate: take environ[0]'s NAME and look it up
     with libc's OWN `getenv`, then require the returned VALUE to be
     byte-identical to the text after environ[0]'s '=' (exit 7 = getenv cannot
     see a variable environ claims exists; exit 8 = the two disagree). A slot
     copied from the wrong object, or copied too early/late, fails this while
     passing every purely structural test above.

   RED-ON-DISABLE, and note the FIRST of these is the one that would otherwise
   have shipped silently: repoint the row (or the macro) at the weak `environ`
   spelling -> this program returns 1 on every elf leg, NOT a diagnostic, which
   is exactly why the run witness exists and a compile-only pin would not have
   caught it. Un-ship the row -> honest S0001; un-ship the macro -> honest
   S0001 `got environ`; drop `dataImportBinding` from the elf exec format ->
   the linker's loud data-import reject (K_FormatLacksImportSupport).
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
