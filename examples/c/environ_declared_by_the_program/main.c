/* POSIX `environ` in the spelling POSIX itself gives it — the PROGRAM declares
   `extern char **environ;` (XBD 8.1: "pointed to by the external variable
   environ"; no POSIX header declares it: <unistd.h> declares optarg, opterr,
   optind and optopt, and nothing else) — on EVERY pair: pe, both ELF, both
   Mach-O. Every reference compiler measured builds and runs this program to
   exit 42: gcc and clang on Linux (x86_64 and aarch64), mingw-w64 and MSVC on
   Windows, Apple clang on both Mac arches.

   HOW EACH PLATFORM ANSWERS THE DECLARATION — three realizations, one meaning:
   - glibc exports `environ` (with `_environ` and `__environ`, one object at one
     address); the declaration binds it through the GOT.
   - libSystem exports `_environ` (Mach-O's spelling of the C name); the same.
   - Neither Windows CRT exports a bare `environ`: both references' <stdlib.h>
     say `#define environ _environ` and `#define _environ (*__p__environ())`, so
     the declaration below is rewritten into a compatible redeclaration of the
     UCRT accessor, and every use reads the CRT's own environment table.

   WHAT EACH GROUP PROVES (bit set on failure; exit 42 only when all pass):
   1  the vector is live and well formed: non-NULL, non-empty, every entry
      NAME=VALUE with a non-empty NAME (a binding one indirection off reads
      garbage and fails the '=' scan instead of passing);
   2  a variable the program SETS through libc appears in the vector read
      through the declaration — glibc and Apple reallocate the vector when a
      NEW name is added, so a binding that read a copy (the deleted
      copy-relocation shape) or cached the pointer would miss it;
   4  getenv's answer for it points INTO that entry (pointer identity, not
      string equality: the declaration and libc name one object);
   8  the WRITE direction: the program installs its own vector through the
      declaration and getenv — libc's internal read — must see it, then the
      program restores the original (UCRT frees its table's entries at exit);
   16 removal is visible: after unsetenv (POSIX) / `_putenv("NAME=")`
      (Windows) the entry is gone from the vector. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L   /* setenv/unsetenv: POSIX, not ISO C */
#endif
#include <stdlib.h>
#include <string.h>

extern char **environ;

static int set_probe(void) {
#if defined(_WIN32)
    return _putenv("DSS_ENVIRON_PROBE=v1-set-by-the-program");
#else
    return setenv("DSS_ENVIRON_PROBE", "v1-set-by-the-program", 1);
#endif
}

static int unset_probe(void) {
#if defined(_WIN32)
    return _putenv("DSS_ENVIRON_PROBE=");
#else
    return unsetenv("DSS_ENVIRON_PROBE");
#endif
}

/* The entry "NAME=..." in the vector the declaration names, or NULL. */
static char *find_entry(char const *name) {
    size_t const n = strlen(name);
    for (char **e = environ; *e != NULL; ++e) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e;
    }
    return NULL;
}

int main(void) {
    int fails = 0;

    /* 1: live and well formed. */
    if (environ == NULL || environ[0] == NULL) {
        fails |= 1;
    } else {
        for (char **e = environ; *e != NULL; ++e) {
            char const *eq = strchr(*e, '=');
            if (eq == NULL || eq == *e) { fails |= 1; break; }
        }
    }

    /* 2: a variable set through libc is visible through the declaration. */
    char *entry = NULL;
    if (set_probe() != 0) {
        fails |= 2;
    } else {
        entry = find_entry("DSS_ENVIRON_PROBE");
        if (entry == NULL || strcmp(entry, "DSS_ENVIRON_PROBE=v1-set-by-the-program") != 0) fails |= 2;
    }

    /* 4: getenv points into that very entry. */
    if (entry != NULL) {
        char const *g = getenv("DSS_ENVIRON_PROBE");
        if (g != entry + strlen("DSS_ENVIRON_PROBE=")) fails |= 4;
    } else {
        fails |= 4;
    }

    /* 8: the write direction — libc reads the vector the program installs. */
    {
        static char w_entry[] = "DSS_ENVIRON_WRITTEN=through-the-declaration";
        char *mine[] = { w_entry, NULL };
        char **const saved = environ;
        environ = mine;
        char const *g = getenv("DSS_ENVIRON_WRITTEN");
        int const seen = (g == w_entry + strlen("DSS_ENVIRON_WRITTEN="));
        environ = saved;
        if (!seen) fails |= 8;
        if (environ != saved) fails |= 8;
    }

    /* 16: removal is visible through the declaration. */
    if (unset_probe() != 0 || find_entry("DSS_ENVIRON_PROBE") != NULL) fails |= 16;

    return fails == 0 ? 42 : fails;
}
