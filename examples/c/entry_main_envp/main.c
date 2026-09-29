/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: the RUN witness for `int main(int, char**,
 * char**)`, the environment form gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0
 * and MSVC 19.51 all hand the process environment (MEASURED 2026-09-24: every
 * entry agreeing with getenv under each reference's own startup).
 *
 * ONE source, and every format realizes the verb its own declared way:
 *   * elf (`stack-vector`) -- envp sits one slot past argv's NULL terminator on the
 *     entry stack, a RUNTIME address; a synthesized init takes the trampoline's
 *     (argc, argv), computes argv + (argc + 1) and calls main;
 *   * pe (`crt-argv-accessors`) -- the synthesized init follows MSVC's own
 *     startup: `_initialize_narrow_environment()`, then
 *     `main(argc, argv, _get_initial_narrow_environment())`;
 *   * macho (no mechanism) -- dyld calls LC_MAIN with envp already in the third
 *     argument register, and the trampoline passes it through.
 *
 * WHAT IS CHECKED. The environment's CONTENT is host-dependent and never asserted
 * -- only its STRUCTURE, plus agreement with libc's own getenv, an oracle that
 * does not read this vector:
 *   90  argc wrong (the runner passes no arguments, so argc == 1);
 *   91  argv broken (NULL, an empty argv[0], or argv[argc] not NULL);
 *   92  envp NULL;
 *   93  an entry with no '=' or an empty NAME -- a vector one slot off, or a
 *       register nobody loaded, walks garbage and fails here instead of passing;
 *   94  getenv(NAME) disagrees with the VALUE of the first entry naming NAME;
 *   95  the vector is EMPTY -- an envp landing on argv's NULL terminator (the
 *       off-by-one-slot class) reads as an empty environment, so empty is refused.
 *
 * THE CONSTRUCTOR IS A SECOND WITNESS. It runs between the program's entry and
 * `main`, and it CALLS a four-argument function, which loads the first four
 * argument registers. On Mach-O dyld delivers envp in the THIRD argument
 * register, so unless the trampoline carries every register a declared entry form
 * reads across the initializers -- not the two it once carried -- `main`
 * receives what fwrite left there and fails at 92 or 93. (On elf and pe the
 * environment is fetched after the initializers, so there the constructor only
 * proves it does not disturb the result.)
 *
 * RED-ON-DISABLE: take the verb away from one format -- "argc-argv-envp" out of its
 * `entryVerbs` together with the declaration only that verb reads (elf's layout
 * keys, pe's narrow environment pair; the format loader refuses either half
 * without the other) -- and that leg refuses the build (K_ProgramEntryUndefined:
 * no candidate survives the intersection); delete the 3-parameter `main` row
 * from c.lang.json and every leg refuses it at main's declarator
 * (S_EntryShapeNotDeclared); make the elf init add argc slots instead of
 * argc + 1 and envp lands on argv's terminator -> 95.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kNameMax = 256 };

__attribute__((constructor)) static void load_four_argument_registers(void) {
    (void)fwrite("", 1, 0, stdout);   /* four arguments; writes nothing */
}

/* Whether an entry before `upto` names `name` (its first `len` bytes). */
static int named_earlier(char **envp, char **upto, char const *name, size_t len) {
    for (char **e = envp; e != upto; ++e) {
        if (strncmp(*e, name, len) == 0 && (*e)[len] == '=') {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
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
    for (char **e = envp; *e; ++e) {
        char const *entry = *e;
        char const *eq = strchr(entry, '=');
        if (!eq || eq == entry) {
            return 93;
        }
        size_t const len = (size_t)(eq - entry);
        ++n;
        if (len >= kNameMax || named_earlier(envp, e, entry, len)) {
            continue;
        }
        char name[kNameMax];
        memcpy(name, entry, len);
        name[len] = '\0';
        char const *value = getenv(name);
        if (!value || strcmp(value, eq + 1) != 0) {
            return 94;
        }
    }
    if (n == 0) {
        return 95;
    }
    return 42;
}
