/* ════════════════════════════════════════════════════════════════════════
   D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET — the RUN witness.

   ★★★ THE GENERAL RULE THIS EXAMPLE EXISTS TO HONOUR:
       AN OBJECT-IDENTITY PROPERTY CANNOT BE WITNESSED BY ONE IMAGE.
   A test asserting that a symbol denotes the SAME OBJECT throughout the
   program MUST involve a SECOND image DSS did not build. This example links
   against a gcc-built `.so` shipped beside it, and every identity assertion
   below crosses that image boundary.

   WHAT WENT WRONG, AND WHY THE OLD WITNESS COULD NOT SEE IT
   ---------------------------------------------------------
   DSS used to bind an ET_EXEC's imported library DATA objects by ELF COPY
   RELOCATION: reserve a `.bss` slot, export the symbol as a DEFINED OBJECT
   there, emit one R_*_COPY, and let ld.so memcpy the library's object in.
   A copy relocation redirects the library's references FOR THE ONE NAME THE
   EXEC CLAIMS. ✔MEASURED: glibc exports ONE environment object under THREE
   names — `__environ` (GLOBAL), `_environ` and `environ` (WEAK) — all at one
   address (0x20ad58 on x86_64 / 0x1b7288 on aarch64). DSS's descriptor names
   `__environ`, so `__libc_start_main` wrote envp into the exec's copy while
   libc's own `environ` slot was NEVER written. A third-party
   `libtcl8.6.so` reading the UN-PREFIXED `environ` therefore saw NULL and
   SIGSEGV'd in `TclSetupEnv+0xe8` (rc=139) — with NO diagnostic from
   anything, and with the linked binary looking perfect under `readelf`.
   C23 6.2.2: an identifier with external linkage denotes THE SAME OBJECT
   throughout the entire program. Copy relocation split one object in two.
   (`environ` itself is POSIX, so the conformance surface here is OBJECT
   IDENTITY, not the symbol.)

   ★ The predecessor witness, `examples/c-subset/shipped_environ`, read
   `environ` through the DSS macro and cross-checked it with libc's own
   `getenv` — but BOTH of those reads went through the very name the exec
   claimed, so it compared the exec's copy WITH ITSELF. A SELF-LOOP,
   structurally unable to fail, born vacuous: it passed green on all four
   legs across the whole regression window (`09e1608a` green → `6f4aab73`
   rc=139).

   THE FIX BEING WITNESSED: every ELF format now declares
   `dataImportBinding: "got-indirect"` and the copy-relocation mechanism is
   DELETED from the vocabulary. An imported object's ADDRESS is loaded from a
   GOT slot ld.so fills, so the exec DEFINES NOTHING and cannot split an
   alias set. (GOT-indirect is legal in a non-PIE ET_EXEC: a non-PIE exec has
   a `.got`, and R_*_GLOB_DAT in `.rela.dyn` is load-time work ld.so does with
   no PIE requirement.)

   THE FIVE THINGS THIS PROGRAM ASSERTS
   ------------------------------------
   (a) ★ THE WITNESS IS VERIFIED BEFORE THE SUBJECT IS. This program PARSES
       the shipped `.so`'s own dynamic relocations and requires at least one
       naming exactly `environ`, and ZERO naming `__environ`. A `.so` that
       happened to reference the PREFIXED spelling would make everything
       below vacuous in exactly the way the old witness was — so the witness
       is checked, not assumed.
       ⚠ It reads the NUL-terminated string at `.dynstr + st_name`, never
       searching `.dynstr` for a standalone "environ": ✔MEASURED, `ld` points
       `environ`'s st_name INTO THE MIDDLE of `dss_probe_environ_object`
       (tail-suffix string sharing), so a substring scan would report a false
       PRESENT for a `.so` that never referenced it. Also note `readelf`
       prints `environ@GLIBC_2.2.5` while the RAW st_name is exactly
       `environ` — the version suffix is synthesized from `.gnu.version` and
       is NOT in the string table.
   (b) A VALUE, NEVER MERE SURVIVAL: the library reports the number of
       entries IT can see and the VALUE it finds for the variable this
       program's own `environ[0]` names, and the two must be byte-identical.
       The library walks `environ` itself — deliberately NOT `getenv`, which
       reads libc's internal `__environ` and would keep agreeing with a copy
       while the un-prefixed object stayed empty.
   (c) ★ THE `if (environ)` ARM, called out separately because it is the
       SILENT half of the defect: a library that merely TESTS the pointer
       sees NULL, takes a wrong branch and never crashes. `-1` from
       `dss_probe_env_count` is exactly that state and gets its own exit code.
   (d) ★ THE MATCHED gcc-REFERENCE CONTROL, in the same runner and the same
       PROCESS: `&environ` as gcc-compiled code binds it must equal
       `&environ` as DSS-compiled code binds it. Two independent compilers
       agreeing on ONE address is what makes a future divergence
       ATTRIBUTABLE rather than argued.
   (e) ★ SAME_OBJECT=YES, the promoted diagnosis instrument: `dlsym` of BOTH
       spellings — `environ` and `__environ` — must return the SAME non-null
       address, and that address must be the one both compilers bound.
       Under the defect this reported `dlsym(environ)` nil vs
       `dlsym(__environ) 0xffffc60c9140`, SAME_OBJECT=NO, while a gcc-built
       control of the same program reported YES. Cheapest permanent pin.

   BOTH ELF ARCHES, and that is not symmetry for its own sake: the bug was
   arm64-VISIBLE and x86_64-LATENT (the x86_64 leg linked and ran the same
   split object without crashing, because nothing on that leg happened to
   read the un-prefixed name). A single-arch witness would have missed this
   one and will miss the next one.

   RED-ON-DISABLE (each MEASURED on this example, none a line-count check):
     * Revert `dataImportBinding` to a copy-relocation-style binding — i.e.
       make the exec DEFINE `__environ` at its own `.bss` — and (d) fires
       first (exit 50: gcc's object address is libc's, DSS's is the exec's
       copy), then (e) exit 62. Nothing crashes; the exit code is the only
       signal, which is exactly why a RUN witness is required and a
       compile-only pin would wave it through.
     * Repoint the shipped `.so` at the PREFIXED spelling and (a) reds at
       exit 13 — the witness refuses to certify itself.
     * Delete the shipped `.so` and (a) reds at exit 10 rather than skipping.
     * Un-ship the `__environ` row or the `environ` macro in unistd.json →
       honest S0001 at compile time.
   elf-ONLY on purpose: no Windows CRT exports a spelling ucrtbase can bind
   (✔MEASURED: ucrtbase has none of the three; msvcrt only `_environ`), and
   the macho export is unmeasured, so both stay fail-loud S0001 per this
   directory's need-driven staging rule.
   ════════════════════════════════════════════════════════════════════════ */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The shipped gcc-built second image, one per ELF arch. Selected by the
   target's own arch predefines — never by a host fact. */
#if defined(__aarch64__)
#define PROBE_SO "./libdss_env_probe_aarch64.so"
#elif defined(__x86_64__)
#define PROBE_SO "./libdss_env_probe_x86_64.so"
#else
#error "environ_alias_object_identity ships a prebuilt .so only for elf64 x86_64 / aarch64"
#endif

typedef long (*CountFn)(void);
typedef char *(*LookupFn)(char *);
typedef char ***(*ObjectFn)(void);

/* Little-endian scalar reads from a raw byte buffer — no struct overlays, so
   nothing here depends on this compiler's alignment or padding choices for a
   FOREIGN file's layout. ELF64/LSB is the only shape either arch produces. */
static unsigned long long rdU(unsigned char *p, int n) {
    unsigned long long v = 0;
    int i;
    for (i = n - 1; i >= 0; --i) {
        v = (v << 8) | (unsigned long long)p[i];
    }
    return v;
}

/* ── (a) VERIFY THE WITNESS: does the shipped `.so` really relocate against
       the UN-PREFIXED `environ`, and never against `__environ`?
   Returns 0 on success, or the exit code to report. */
static int verifyProbeSoRelocatesUnprefixedEnviron(char *path) {
    FILE          *f;
    long           len;
    unsigned char *buf;
    unsigned long long shoff;
    unsigned long long shentsize;
    unsigned long long shnum;
    unsigned long long i;
    unsigned long long j;
    long           hits = 0;    /* relocations naming exactly "environ"   */
    long           dunder = 0;  /* relocations naming exactly "__environ" */
    long           relaSections = 0;
    int            rc = 0;

    f = fopen(path, "rb");
    if (f == 0) return 10;                  /* the witness is ABSENT: fail, never skip */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 10; }
    len = ftell(f);
    if (len < 64) { fclose(f); return 11; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 10; }
    buf = (unsigned char *)malloc((unsigned long)len);
    if (buf == 0) { fclose(f); return 10; }
    if (fread(buf, 1, (unsigned long)len, f) != (unsigned long)len) {
        free(buf); fclose(f); return 10;
    }
    fclose(f);

    /* ELF64 little-endian only: \x7f E L F, EI_CLASS=2, EI_DATA=1. */
    if (buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F'
        || buf[4] != 2 || buf[5] != 1) {
        free(buf); return 11;
    }
    shoff     = rdU(buf + 40, 8);
    shentsize = rdU(buf + 58, 2);
    shnum     = rdU(buf + 60, 2);
    if (shentsize != 64 || shnum == 0
        || shoff + shnum * shentsize > (unsigned long long)len) {
        free(buf); return 12;               /* no section headers to read */
    }

    for (i = 0; i < shnum; ++i) {
        unsigned char     *sh = buf + shoff + i * shentsize;
        unsigned long long shType   = rdU(sh + 4, 4);
        unsigned long long shOffset = rdU(sh + 24, 8);
        unsigned long long shSize   = rdU(sh + 32, 8);
        unsigned long long shLink   = rdU(sh + 40, 4);
        unsigned char     *symSh;
        unsigned long long symOff;
        unsigned long long symSize;
        unsigned long long strIdx;
        unsigned char     *strSh;
        unsigned long long strOff;
        unsigned long long strSize;

        if (shType != 4) continue;           /* SHT_RELA */
        ++relaSections;
        if (shLink >= shnum) { rc = 12; break; }
        symSh   = buf + shoff + shLink * shentsize;
        symOff  = rdU(symSh + 24, 8);
        symSize = rdU(symSh + 32, 8);
        strIdx  = rdU(symSh + 40, 4);
        if (strIdx >= shnum) { rc = 12; break; }
        strSh   = buf + shoff + strIdx * shentsize;
        strOff  = rdU(strSh + 24, 8);
        strSize = rdU(strSh + 32, 8);
        if (shOffset + shSize > (unsigned long long)len
            || symOff + symSize > (unsigned long long)len
            || strOff + strSize > (unsigned long long)len) {
            rc = 12; break;
        }
        /* Elf64_Rela is 24 bytes: r_offset, r_info, r_addend.
           The symbol index is the HIGH 32 bits of r_info. */
        for (j = 0; j + 24 <= shSize; j += 24) {
            unsigned char     *ra   = buf + shOffset + j;
            unsigned long long info = rdU(ra + 8, 8);
            unsigned long long symIdx = info >> 32;
            unsigned char     *sym;
            unsigned long long nameOff;
            char              *nm;

            if (symIdx == 0) continue;                  /* no symbol */
            if ((symIdx + 1) * 24 > symSize) { rc = 12; break; }
            sym     = buf + symOff + symIdx * 24;       /* Elf64_Sym is 24 bytes */
            nameOff = rdU(sym + 0, 4);
            if (nameOff >= strSize) { rc = 12; break; }
            /* ★ the NUL-terminated string AT the offset — see the header note
               on `ld`'s tail-suffix string sharing. */
            nm = (char *)(buf + strOff + nameOff);
            if (strcmp(nm, "environ") == 0)   ++hits;
            if (strcmp(nm, "__environ") == 0) ++dunder;
        }
        if (rc != 0) break;
    }
    free(buf);
    if (rc != 0) return rc;
    if (relaSections == 0) return 12;   /* fail-closed: nothing was inspected */
    if (hits == 0)   return 13;         /* ★ the witness does not witness */
    if (dunder != 0) return 14;         /* ★ it reads the PREFIXED spelling */
    return 0;
}

int main(void) {
    void    *h;
    void    *globalHandle;
    CountFn  probeCount;
    LookupFn probeLookup;
    ObjectFn probeObject;
    void    *symUnprefixed;
    void    *symPrefixed;
    char  ***gccObject;
    char  ***dssObject;
    long     libCount;
    long     ownCount;
    char   **p;
    char    *entry;
    char    *eq;
    char    *scan;
    char    *libVal;
    char    *ownVal;
    char     name[256];
    long     nameLen;
    long     i;
    int      vrc;

    /* (a) The witness must witness — checked BEFORE anything is trusted. */
    vrc = verifyProbeSoRelocatesUnprefixedEnviron(PROBE_SO);
    if (vrc != 0) {
        fprintf(stderr, "witness verification failed on %s (code %d)\n",
                PROBE_SO, vrc);
        return vrc;
    }

    h = dlopen(PROBE_SO, RTLD_NOW | RTLD_GLOBAL);
    if (h == 0) { fprintf(stderr, "dlopen failed: %s\n", PROBE_SO); return 20; }
    probeCount  = (CountFn)dlsym(h, "dss_probe_env_count");
    probeLookup = (LookupFn)dlsym(h, "dss_probe_env_lookup");
    probeObject = (ObjectFn)dlsym(h, "dss_probe_environ_object");
    if (probeCount == 0 || probeLookup == 0 || probeObject == 0) return 21;

    /* (c) ★ THE `if (environ)` ARM — the SILENT half. A library that merely
       tests the pointer sees NULL, branches wrong and never crashes. */
    libCount = probeCount();
    if (libCount < 0) {
        fprintf(stderr, "the library sees environ == NULL: the object is "
                        "SPLIT (no crash, wrong branch)\n");
        return 30;
    }
    if (libCount == 0) return 31;          /* visible but EMPTY */

    if (environ == 0) return 32;           /* this image's own view */
    ownCount = 0;
    for (p = environ; *p != 0; ++p) ++ownCount;
    if (ownCount != libCount) {
        fprintf(stderr, "counts disagree across the image boundary: "
                        "exec %ld vs library %ld\n", ownCount, libCount);
        return 33;                          /* two objects, both non-empty */
    }

    /* (b) A VALUE across the image boundary, not survival: take this image's
       environ[0] NAME and require the LIBRARY's own walk of `environ` to
       report a byte-identical VALUE. */
    entry = environ[0];
    eq = 0;
    for (scan = entry; *scan != 0; ++scan) {
        if (*scan == '=') { eq = scan; break; }
    }
    if (eq == 0 || eq == entry) return 34;  /* not a NAME=VALUE vector */
    nameLen = eq - entry;
    if (nameLen > 255) return 35;
    for (i = 0; i < nameLen; ++i) name[i] = entry[i];
    name[nameLen] = 0;
    ownVal = eq + 1;
    libVal = probeLookup(name);
    if (libVal == 0) {
        fprintf(stderr, "the library cannot see '%s', which this image's "
                        "environ[0] names\n", name);
        return 40;
    }
    for (i = 0; libVal[i] != 0 || ownVal[i] != 0; ++i) {
        if (libVal[i] != ownVal[i]) return 41;
    }

    /* (d) ★ THE MATCHED gcc-REFERENCE CONTROL: one address, two compilers. */
    gccObject = probeObject();
    dssObject = &environ;
    if (gccObject != dssObject) {
        fprintf(stderr, "SAME_OBJECT=NO — gcc binds &environ at %p, DSS at "
                        "%p: two objects for one identifier (C23 6.2.2)\n",
                (void *)gccObject, (void *)dssObject);
        return 50;
    }

    /* (e) ★ SAME_OBJECT=YES through the loader itself: both spellings of the
       alias set must resolve to the ONE object both compilers bound.
       ★ THE HANDLE MUST BE THE GLOBAL ONE — `dlopen(NULL, ...)` — NOT the
       probe library's. ✔MEASURED, and it cost a run to learn: `dlsym` on a
       SPECIFIC library handle searches that library and its dependencies
       only, so it reaches libc's ORIGINAL storage and skips the executable
       entirely. Asked that way, a matched `gcc -no-pie` control FAILS this
       check too — gcc copy-relocates `environ` into its own `.bss`, so the
       exec's definition is the live object and libc's is the stale one. A
       check a correct reference compiler fails is a broken check, not a
       finding; the global handle is the scope the question is about. */
    globalHandle = dlopen((void *)0, RTLD_NOW);
    if (globalHandle == 0) return 64;
    symUnprefixed = dlsym(globalHandle, "environ");
    symPrefixed   = dlsym(globalHandle, "__environ");
    if (symUnprefixed == 0) return 60;
    if (symPrefixed == 0) return 61;
    if (symUnprefixed != symPrefixed) {
        fprintf(stderr, "SAME_OBJECT=NO — dlsym(environ)=%p vs "
                        "dlsym(__environ)=%p\n", symUnprefixed, symPrefixed);
        return 62;
    }
    if (symUnprefixed != (void *)dssObject) {
        fprintf(stderr, "the loader's object %p is not the one the two "
                        "compilers bound (%p)\n",
                symUnprefixed, (void *)dssObject);
        return 63;
    }

    puts("environ:one-object");
    if (fflush(0) != 0) return 9;
    dlclose(h);
    return 42;
}
