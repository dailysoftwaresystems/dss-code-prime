/* ════════════════════════════════════════════════════════════════════════
   THE SECOND IMAGE — the source of the PREBUILT, gcc-BUILT `.so` that ships
   beside it (`libdss_env_probe_x86_64.so`, `libdss_env_probe_aarch64.so`).

   ★★ WHY A SECOND IMAGE AT ALL, which is the whole reason this file exists:
   AN OBJECT-IDENTITY PROPERTY CANNOT BE WITNESSED BY ONE IMAGE. `main.c`'s
   predecessor (`examples/c-subset/shipped_environ`) read `environ` through
   the DSS macro and then checked it against libc's own `getenv` — but with a
   copy relocation BOTH of those reads went through the very name the exec
   claimed, so the test compared the exec's copy with itself. It was a
   SELF-LOOP, structurally unable to fail, and it passed on all four legs
   while a third-party `libtcl8.6.so` reading the UN-PREFIXED `environ` got
   NULL and SIGSEGV'd in `TclSetupEnv+0xe8`
   (D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET).
   A test asserting that a symbol denotes the SAME OBJECT must therefore
   involve a SECOND image DSS did not build. This is that image.

   WHY IT IS SHIPPED PREBUILT: so the example needs no host cross-compiler at
   test time. Both arches are committed as binaries; the build command that
   produced them is recorded at the bottom of this comment and in
   `expected.json`.

   ★ AND WHY `main.c` VERIFIES THIS FILE'S OWN BINARY BEFORE TRUSTING IT: a
   `.so` that happened to reference `__environ` (the PREFIXED spelling) would
   make the whole witness vacuous in exactly the way the old one was. So
   main.c PARSES the shipped `.so`'s dynamic symbol table and requires an
   UNDEFINED symbol spelled exactly `environ`, with NO `__environ` anywhere in
   it. Verify the witness, not just the subject.

   BUILD (from this directory, in WSL / any glibc Linux):
     gcc            -shared -fPIC -O2 -std=gnu11 -o libdss_env_probe_x86_64.so  env_probe_so.c
     aarch64-linux-gnu-gcc -shared -fPIC -O2 -std=gnu11 -o libdss_env_probe_aarch64.so env_probe_so.c
   ════════════════════════════════════════════════════════════════════════ */
#include <stddef.h>
#include <string.h>

/* ★ THE UN-PREFIXED POSIX SPELLING. This single declaration is the subject of
   the whole witness: glibc exports ONE object under THREE names (`__environ`
   GLOBAL, `environ` / `_environ` WEAK, all at one address — measured at
   0x20ad58 on x86_64), and DSS's descriptor names only `__environ`. If DSS
   ever again binds an imported data object by a mechanism that claims ONE
   name (a copy relocation), this `environ` resolves to storage nobody wrote
   and every function below reports an EMPTY environment. glibc gates the
   plain spelling behind __USE_GNU, hence the explicit extern rather than
   <unistd.h>. */
extern char **environ;

/* (1) THE LIBRARY'S OWN VIEW, as a COUNT rather than a survival check.
   -1 means the POINTER ITSELF is null — the copy-relocation split symptom,
   and precisely the case a library that merely writes `if (environ)` handles
   by taking a wrong branch with no crash at all. */
long dss_probe_env_count(void) {
    char **p;
    long   n = 0;
    if (environ == NULL) return -1;
    for (p = environ; *p != NULL; ++p) ++n;
    return n;
}

/* (2) A VALUE, never mere survival: walk `environ` ITSELF and return the text
   after '=' for `name`, or NULL.
   ★ DELIBERATELY NOT `getenv`: getenv reads libc's INTERNAL `__environ`, so it
   would keep agreeing with the exec's copy while THIS object stayed empty —
   the same self-loop that made the old witness vacuous. */
char const *dss_probe_env_lookup(char const *name) {
    char **p;
    size_t n;
    if (environ == NULL || name == NULL) return NULL;
    n = strlen(name);
    for (p = environ; *p != NULL; ++p) {
        if (strncmp(*p, name, n) == 0 && (*p)[n] == '=') return *p + n + 1;
    }
    return NULL;
}

/* (3) THE MATCHED gcc-REFERENCE CONTROL, in the same runner and the same
   PROCESS: the ADDRESS of the object THIS gcc-compiled code binds `environ`
   to. main.c compares it against the address DSS-compiled code binds — so
   "same object" is decided by two independent compilers agreeing on one
   address, not argued. A future divergence is attributable by construction:
   if these differ, one of the two images is claiming a name the other is not.
   Returns `&environ`, NOT `environ` — the OBJECT's address, which is what
   identity is about; its VALUE changes whenever anyone calls setenv. */
char ***dss_probe_environ_object(void) { return &environ; }
