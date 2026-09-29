/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: the RUN witness for MSVC's 3-parameter
 * `wmain(int, wchar_t**, wchar_t**)`, whose third vector is the WIDE environment
 * -- MSVC's own startup passes `_get_initial_wide_environment()` (DOCUMENTED, the
 * toolset's crt/src/vcruntime/exe_common.inl). The entry matches the language row
 * `fn(i32, ptr-ptr-u16, ptr-ptr-u16) -> i32`, whose verb `argc-wargv-wenvp` the pe
 * format realizes through the UCRT's wide environment pair: the synthesized init
 * calls `_initialize_wide_environment()`, then passes
 * `_get_initial_wide_environment()` beside the wide argv.
 *
 * The params are spelled `unsigned short**`, a STRUCTURAL shape, because
 * `wchar_t` is 16-bit on Windows and 32-bit elsewhere (see c.lang.json).
 *
 * WHAT IS CHECKED -- structure, plus a CROSS-WIDTH agreement: for every entry
 * whose name and value are plain ASCII, the NARROW `getenv(name)` must return the
 * same text. The UCRT keeps the narrow and wide environments as SEPARATE tables,
 * so a wide vector that is not the process environment disagrees with an oracle
 * that never reads it.
 *   90  argc wrong (the runner passes no arguments, so argc == 1);
 *   91  wide argv broken (NULL, an empty argv[0], or argv[argc] not NULL);
 *   92  wenvp NULL;
 *   93  an entry with no '=' or an empty NAME;
 *   94  the narrow getenv disagrees with an all-ASCII wide entry;
 *   95  the vector is EMPTY;
 *   96  no entry was all-ASCII, so nothing was compared -- refused rather than
 *       reported as agreement.
 *
 * RED-ON-DISABLE: take "argc-wargv-wenvp" out of pe64-x86_64-windows-exec's
 * `entryVerbs` together with the wide environment pair only it reads (the format
 * loader refuses either half alone) -> K_ProgramEntryUndefined; hand the init the
 * NARROW accessor for the wide verb -> the u16 walk reads byte pairs and fails at
 * 93 or 94.
 */
#include <stdlib.h>
#include <string.h>

enum { kNameMax = 256, kValueMax = 4096 };

static size_t wide_length(unsigned short const *w) {
    size_t n = 0;
    while (w[n]) {
        ++n;
    }
    return n;
}

/* Copy the `n` units at `w` into `out` as ASCII; 0 if one is not ASCII or they
   do not fit in `cap` bytes with the terminator. */
static int ascii_narrow(unsigned short const *w, size_t n, char *out, size_t cap) {
    if (n + 1 > cap) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        if (w[i] == 0 || w[i] > 0x7F) {
            return 0;
        }
        out[i] = (char)w[i];
    }
    out[n] = '\0';
    return 1;
}

int wmain(int argc, unsigned short **argv, unsigned short **envp) {
    if (argc != 1) {
        return 90;
    }
    if (!argv || !argv[0] || !argv[0][0] || argv[argc]) {
        return 91;
    }
    if (!envp) {
        return 92;
    }
    int n = 0;
    int compared = 0;
    for (unsigned short **e = envp; *e; ++e) {
        unsigned short const *entry = *e;
        size_t eq = 0;
        while (entry[eq] && entry[eq] != '=') {
            ++eq;
        }
        if (!entry[eq] || eq == 0) {
            return 93;
        }
        ++n;
        char name[kNameMax];
        char value[kValueMax];
        unsigned short const *wide_value = entry + eq + 1;
        if (!ascii_narrow(entry, eq, name, sizeof name)
            || !ascii_narrow(wide_value, wide_length(wide_value), value, sizeof value)) {
            continue;
        }
        char const *narrow = getenv(name);
        if (!narrow || strcmp(narrow, value) != 0) {
            return 94;
        }
        ++compared;
    }
    if (n == 0) {
        return 95;
    }
    if (compared == 0) {
        return 96;
    }
    return 42;
}
