/* ═══ DSS PLATFORM RUNTIME — <getopt.h> for the `pe` object format ═══════════
 *
 * The IMPLEMENTATION half of `getopt`, `getopt_long` and `getopt_long_only` and of
 * the four globals `optarg`/`optind`/`opterr`/`optopt`. The DECLARATION half is
 * `src/dss-config/shippedLibs/getopt.json` (and the POSIX subset `unistd.json`
 * re-declares); each descriptor's `realization` map names THIS file for `pe`,
 * exactly as `dirent.json` names `dirent.c`.
 *
 * ★ WHY A BODY. No Windows platform image exports any of these names under any
 * spelling — not ucrtbase, not kernel32; mingw-w64 supplies them from its own
 * static runtime (libmingwex). A declaration bound to a DLL would break every
 * pe binary's LOAD, because DSS imports each declared shipped extern eagerly.
 * On `elf` and `macho` the descriptors carry no realization and the platform C
 * library's own getopt is imported — the reference's, by construction.
 *
 * ★★ THE BEHAVIOUR IS mingw-w64's, MEASURED, because mingw is the pe reference
 * that ships these names (MSVC ships no <getopt.h>). ✔MEASURED 2026-09-23 against
 * x86_64-w64-mingw32-gcc 13.2.0 by a 13-scenario probe, and this file linked into
 * the same probe in place of mingw's gives an IDENTICAL transcript, messages
 * included; examples/c/getopt_platform runs it on every target:
 *   - `getopt` does NOT permute: it stops at the first operand (glibc's does
 *     permute; Darwin's does not). `getopt_long`/`getopt_long_only` DO permute —
 *     operands are moved after the options, in their original order — unless
 *     the option string starts with `+` or POSIXLY_CORRECT is set; a leading
 *     `-` returns each operand in order as option 1 with `optarg` = the operand.
 *   - `--` ends the options (consumed); a lone `-` is an operand unless `-` is
 *     in the option string.
 *   - A leading `:` (after any `+`/`-`) silences messages and makes a missing
 *     argument return `:`; so does a long option given `=value` that takes none.
 *   - A long option may be abbreviated to any unique prefix; prefixes shared by
 *     entries that would ALL do the same thing (same has_arg, flag and val) are
 *     not ambiguous, except under `getopt_long_only`. `getopt_long_only` also
 *     accepts a single dash, and falls back to a short option when a one-letter
 *     token names one.
 *   - `-W foo` is `--foo` when the option string holds `W;`.
 *   - Messages go to stderr as `<program>: <text> -- <what>`, where <program> is
 *     the PROCESS's argv[0] (`__argv[0]`), not the vector passed in.
 *   - `optind = 0` re-initializes a scan (the GNU convention mingw follows).
 *   - Initial values: optind 1, opterr 1, optopt '?', optarg NULL.
 *
 * ★★ THE SIGNATURES ARE CHECKED BY THE COMPILER: this unit includes <getopt.h>,
 * so every definition below must match the descriptor row that publishes it.
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int   optind = 1;
int   opterr = 1;
int   optopt = '?';
char *optarg = NULL;

/* How one call scans. PERMUTE: move operands after the options; IN_ORDER: return
 * each operand as option 1; LONG_ONLY: a single dash may introduce a long option. */
enum { DSS_PERMUTE = 1, DSS_IN_ORDER = 2, DSS_LONG_ONLY = 4 };

/* The scan state that outlives one call. `dss_next` points inside the argument
 * being taken apart (a group like `-xvf`); an empty string means "take the next
 * argument". [dss_skip_first, dss_skip_end) is the run of operands passed over
 * while permuting, still waiting to be moved behind the options. */
static char const  dss_empty[] = "";
static char const *dss_next = dss_empty;
static int         dss_skip_first = -1;
static int         dss_skip_end = -1;
static int         dss_posix = -1;   /* POSIXLY_CORRECT, read on the first call of a scan */

static char const *dss_program_name(void) {
    char **const av = __argv;
    if (av != NULL && av[0] != NULL) return av[0];
    return "";
}

static void dss_complain(char const *text, char const *what, int what_len) {
    if (what_len < 0) {
        fprintf(stderr, "%s: %s -- %s\n", dss_program_name(), text, what);
    } else {
        fprintf(stderr, "%s: %s -- %.*s\n", dss_program_name(), text, what_len, what);
    }
}

static void dss_complain_char(char const *text, int c) {
    fprintf(stderr, "%s: %s -- %c\n", dss_program_name(), text, c);
}

/* Reverse argv[from, to) in place. */
static void dss_reverse(char **argv, int from, int to) {
    while (from < to - 1) {
        char *const t = argv[from];
        argv[from] = argv[to - 1];
        argv[to - 1] = t;
        ++from;
        --to;
    }
}

/* Move the operand run [first, mid) behind the option run [mid, end), keeping the
 * order inside each run: a rotation, done as three reversals. */
static void dss_rotate(char *const *cargv, int first, int mid, int end) {
    char **const argv = (char **)cargv;
    dss_reverse(argv, first, mid);
    dss_reverse(argv, mid, end);
    dss_reverse(argv, first, end);
}

/* The end of the scan: put any pending operands behind the options and leave
 * `optind` at the first of them. */
static int dss_finish(char *const *argv) {
    if (dss_skip_end != -1) {
        dss_rotate(argv, dss_skip_first, dss_skip_end, optind);
        optind -= dss_skip_end - dss_skip_first;
    } else if (dss_skip_first != -1) {
        optind = dss_skip_first;
    }
    dss_skip_first = -1;
    dss_skip_end = -1;
    dss_next = dss_empty;
    return -1;
}

/* A long option, named by `name` (the text after the dashes, possibly with
 * `=value`). `short_too` says the same letter is a short option (long-only mode),
 * which changes two things: a one-letter prefix never abbreviates, and a name
 * that matches nothing is handed back to the short parser (return -2). */
static int dss_parse_long(int argc, char *const *argv, char const *spec, int report,
                          struct option const *longs, int *long_index, int short_too,
                          int mode, char const *name) {
    char const *const eq = strchr(name, '=');
    size_t const len = eq != NULL ? (size_t)(eq - name) : strlen(name);
    int match = -1;
    int exact = 0;
    int ambiguous = 0;
    int bad_arg = (*spec == ':') ? ':' : '?';

    ++optind;
    for (int i = 0; longs[i].name != NULL; ++i) {
        if (strncmp(name, longs[i].name, len) != 0) continue;
        if (strlen(longs[i].name) == len) {
            match = i;
            exact = 1;
            break;
        }
        if (short_too && len == 1) continue;
        if (match == -1) {
            match = i;
        } else if ((mode & DSS_LONG_ONLY) != 0 || longs[i].has_arg != longs[match].has_arg
                   || longs[i].flag != longs[match].flag || longs[i].val != longs[match].val) {
            ambiguous = 1;
        }
    }
    if (!exact && ambiguous) {
        if (report) dss_complain("ambiguous option", name, (int)len);
        optopt = 0;
        return '?';
    }
    if (match == -1) {
        if (short_too) {
            --optind;
            return -2;
        }
        if (report) dss_complain("unknown option", name, -1);
        optopt = 0;
        return '?';
    }
    struct option const *const o = &longs[match];
    if (o->has_arg == no_argument && eq != NULL) {
        if (report) dss_complain("option doesn't take an argument", name, (int)len);
        optopt = o->flag == NULL ? o->val : 0;
        return bad_arg;
    }
    if (o->has_arg == required_argument || o->has_arg == optional_argument) {
        if (eq != NULL) {
            optarg = (char *)(eq + 1);
        } else if (o->has_arg == required_argument) {
            /* The next argument, whatever it looks like; argv[argc] is NULL. */
            optarg = optind < argc ? argv[optind] : NULL;
            ++optind;
        }
    }
    if (o->has_arg == required_argument && optarg == NULL) {
        if (report) dss_complain("option requires an argument", name, -1);
        optopt = o->flag == NULL ? o->val : 0;
        --optind;
        return bad_arg;
    }
    if (long_index != NULL) *long_index = match;
    if (o->flag != NULL) {
        *o->flag = o->val;
        return 0;
    }
    return o->val;
}

static int dss_getopt(int argc, char *const *argv, char const *spec,
                      struct option const *longs, int *long_index, int mode) {
    if (spec == NULL) return -1;
    int reset = 0;
    if (optind == 0) {
        optind = 1;
        reset = 1;
    }
    if (reset || dss_posix == -1) dss_posix = getenv("POSIXLY_CORRECT") != NULL;
    if (*spec == '-') {
        mode |= DSS_IN_ORDER;
    } else if (dss_posix || *spec == '+') {
        mode &= ~DSS_PERMUTE;
    }
    if (*spec == '+' || *spec == '-') ++spec;
    int const report = opterr && *spec != ':';
    int const bad_arg = (*spec == ':') ? ':' : '?';
    optarg = NULL;
    if (reset) {
        dss_skip_first = -1;
        dss_skip_end = -1;
        dss_next = dss_empty;
    }

    char const *arg_start = NULL;
    if (reset || *dss_next == '\0') {
        for (;;) {
            if (optind >= argc) return dss_finish(argv);
            char const *const a = argv[optind];
            int const operand = a[0] != '-' || (a[1] == '\0' && strchr(spec, '-') == NULL);
            if (!operand) break;
            if (mode & DSS_IN_ORDER) {
                optarg = argv[optind++];
                return 1;
            }
            if (!(mode & DSS_PERMUTE)) return -1;
            if (dss_skip_first == -1) {
                dss_skip_first = optind;
            } else if (dss_skip_end != -1) {
                dss_rotate(argv, dss_skip_first, dss_skip_end, optind);
                dss_skip_first = optind - (dss_skip_end - dss_skip_first);
                dss_skip_end = -1;
            }
            ++optind;
        }
        if (dss_skip_first != -1 && dss_skip_end == -1) dss_skip_end = optind;
        arg_start = argv[optind];
        dss_next = arg_start;
        if (arg_start[1] != '\0') {
            ++dss_next;                                  /* past the dash */
            if (dss_next[0] == '-' && dss_next[1] == '\0') {
                ++optind;                                /* `--`: the end */
                dss_next = dss_empty;
                if (dss_skip_end != -1) {
                    dss_rotate(argv, dss_skip_first, dss_skip_end, optind);
                    optind -= dss_skip_end - dss_skip_first;
                }
                dss_skip_first = -1;
                dss_skip_end = -1;
                return -1;
            }
        }
    }

    /* A long option: `--name`, or `-name` under getopt_long_only — never a
     * lone `-`, and never a group already partly consumed. */
    if (longs != NULL && arg_start != NULL && dss_next != arg_start
        && (*dss_next == '-' || (mode & DSS_LONG_ONLY))) {
        int short_too = 0;
        if (*dss_next == '-') {
            ++dss_next;
        } else if (*dss_next != ':' && strchr(spec, *dss_next) != NULL) {
            short_too = 1;
        }
        int const r = dss_parse_long(argc, argv, spec, report, longs, long_index, short_too,
                                     mode, dss_next);
        if (r != -2) {
            dss_next = dss_empty;
            return r;
        }
    }

    int const c = (unsigned char)*dss_next++;
    char const *const found = (c == ':' || c == '\0') ? NULL : strchr(spec, c);
    if (c == ':' || (c == '-' && *dss_next != '\0') || found == NULL) {
        if (c == '-' && *dss_next == '\0') return -1;
        if (*dss_next == '\0') ++optind;
        if (report) dss_complain_char("unknown option", c);
        optopt = c;
        return '?';
    }
    if (longs != NULL && c == 'W' && found[1] == ';') {
        /* `-W name` is `--name`. */
        char const *name = dss_next;
        if (*name == '\0') {
            if (++optind >= argc) {
                dss_next = dss_empty;
                if (report) dss_complain_char("option requires an argument", c);
                optopt = c;
                return bad_arg;
            }
            name = argv[optind];
        }
        int const r = dss_parse_long(argc, argv, spec, report, longs, long_index, 0, mode,
                                     name);
        dss_next = dss_empty;
        return r;
    }
    if (found[1] != ':') {
        if (*dss_next == '\0') ++optind;
        return c;
    }
    optarg = NULL;
    if (*dss_next != '\0') {
        optarg = (char *)dss_next;                      /* attached: `-ovalue` */
    } else if (found[2] != ':') {                       /* required, separate */
        if (++optind >= argc) {
            dss_next = dss_empty;
            if (report) dss_complain_char("option requires an argument", c);
            optopt = c;
            return bad_arg;
        }
        optarg = argv[optind];
    }
    dss_next = dss_empty;
    ++optind;
    return c;
}

int getopt(int argc, char *const *argv, char const *optstring) {
    /* POSIX getopt: no permutation — mingw-w64's, measured. */
    return dss_getopt(argc, argv, optstring, NULL, NULL, 0);
}

int getopt_long(int argc, char *const *argv, char const *optstring,
                struct option const *longopts, int *longindex) {
    return dss_getopt(argc, argv, optstring, longopts, longindex, DSS_PERMUTE);
}

int getopt_long_only(int argc, char *const *argv, char const *optstring,
                     struct option const *longopts, int *longindex) {
    return dss_getopt(argc, argv, optstring, longopts, longindex,
                      DSS_PERMUTE | DSS_LONG_ONLY);
}
