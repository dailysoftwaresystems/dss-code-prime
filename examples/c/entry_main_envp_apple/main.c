/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: the RUN witness for Darwin's four-parameter
 * `int main(int, char**, char**, char**)`. dyld calls an LC_MAIN entry with
 * (argc, argv, envp, apple) in the first four argument registers, `apple` being a
 * NULL-terminated vector of "key=value" strings the kernel and dyld hand the
 * program (the executable's path among them, as `executable_path=`). The Mach-O
 * formats realize the verb `argc-argv-envp-apple` by PASS-THROUGH -- nothing is
 * emitted -- so the one obligation left is the trampoline's: keep all four
 * registers intact across the before-entry static initializers.
 *
 * THE CONSTRUCTOR IS WHAT MAKES THAT OBSERVABLE. It runs between dyld's call and
 * `main`, and it CALLS a four-argument function, loading the first four argument
 * registers. A trampoline that parks fewer than four hands `main` what fwrite
 * left in envp's and apple's registers, and this fails at 92..97.
 *
 * WHAT IS CHECKED -- structure plus independent agreement, never content:
 *   90  argc wrong (the runner passes no arguments, so argc == 1);
 *   91  argv broken (NULL, an empty argv[0], or argv[argc] not NULL);
 *   92  envp NULL;
 *   93  an envp entry with no '=' or an empty NAME;
 *   94  getenv(NAME) disagrees with the VALUE of the first entry naming NAME;
 *   95  envp EMPTY;
 *   96  apple NULL, empty, or the same vector as envp (a fourth register copied
 *       from the third is not Darwin's vector);
 *   97  no apple entry starts with `executable_path=`.
 *
 * DARWIN-ONLY: on ELF and pe no loader or CRT hands an entry a fourth vector, so
 * those formats do not realize the verb and the same source is refused there
 * (examples/c/entry_main_apple_refused_off_darwin).
 *
 * RED-ON-DISABLE: park only two argument registers in the entry trampoline (the
 * literal this row replaced) -> 92..97 on both Mach-O legs; drop
 * "argc-argv-envp-apple" from a Mach-O format's `entryVerbs` ->
 * K_ProgramEntryUndefined there.
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

int main(int argc, char **argv, char **envp, char **apple) {
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
    if (!apple || !apple[0] || apple == envp) {
        return 96;
    }
    for (char **a = apple; *a; ++a) {
        if (strncmp(*a, "executable_path=", 16) == 0) {
            return 42;
        }
    }
    return 97;
}
