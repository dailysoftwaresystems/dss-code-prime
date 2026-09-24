/* <getopt.h> on every platform DSS targets (P68 round 9,
 * D-CONFIG-NO-SHIPPED-GETOPT-ON-ANY-FORMAT).
 *
 * On Linux and macOS the platform C library's own getopt answers; on Windows no
 * platform image exports it, so DSS builds it from shipped source, written to
 * mingw-w64's measured behaviour. The argument vectors are built here, so the
 * program needs no command line. Each group sets one bit; the exit is 42 only when
 * all six hold, otherwise the bitmask of the groups that did.
 *
 *   1  getopt_long over an argv with operands between the options: a short option,
 *      a long option with `=value`, a short option with a separate argument, a long
 *      option with an optional argument — returned in order, with optarg
 *   2  a missing required argument returns ':' (leading ':' in the option string)
 *      and sets optopt
 *   4  after the scan the operands are PERMUTED behind the options, in their order,
 *      and optind is the first of them
 *   8  plain getopt over an options-first argv: two options, then optind at the
 *      first operand
 *  16  plain getopt over interleaved operands: glibc permutes (GNU); mingw-w64 and
 *      Darwin stop at the first operand (POSIX) — each platform's own answer
 *  32  a `struct option` with a flag pointer: getopt_long returns 0 and stores val
 */
#include <getopt.h>
#include <string.h>

/* Start a new scan: optind = 0 re-initializes glibc's and mingw-w64's getopt;
 * Darwin's plain getopt needs its `optreset`. */
static void newScan(void) {
#if defined(__APPLE__)
    optreset = 1;
    optind = 1;
#else
    optind = 0;
#endif
}

static int longScan(void) {
    char p[] = "prog", a[] = "-a", f1[] = "file1", b[] = "--beta=x", c[] = "-c", cv[] = "cval",
         f2[] = "file2", g[] = "--gamma", c2[] = "-c";
    char *argv[] = {p, a, f1, b, c, cv, f2, g, c2, 0};
    static struct option const longs[] = {
        {"alpha", no_argument, 0, 'a'},
        {"beta", required_argument, 0, 'b'},
        {"gamma", optional_argument, 0, 'g'},
        {0, 0, 0, 0},
    };
    int bits = 0;
    int seq = 0;
    int idx = -1;
    int ch;
    newScan();
    opterr = 0;
    while ((ch = getopt_long(9, argv, ":ab:c:g::", longs, &idx)) != -1) {
        switch (seq++) {
            case 0: if (ch != 'a' || optarg != 0) seq = 100; break;
            case 1: if (ch != 'b' || optarg == 0 || strcmp(optarg, "x") != 0 || idx != 1) seq = 100; break;
            case 2: if (ch != 'c' || optarg == 0 || strcmp(optarg, "cval") != 0) seq = 100; break;
            case 3: if (ch != 'g' || optarg != 0 || idx != 2) seq = 100; break;
            case 4:
                if (ch == ':' && optopt == 'c') bits |= 2;
                break;
            default: seq = 100; break;
        }
    }
    if (seq == 5) bits |= 1;
    if (optind == 7 && strcmp(argv[7], "file1") == 0 && strcmp(argv[8], "file2") == 0
        && strcmp(argv[1], "-a") == 0 && strcmp(argv[2], "--beta=x") == 0) {
        bits |= 4;
    }
    return bits;
}

static int shortScans(void) {
    int bits = 0;
    {
        char p[] = "prog", x[] = "-x", y[] = "-y", yv[] = "yv", o1[] = "op1", o2[] = "op2";
        char *argv[] = {p, x, y, yv, o1, o2, 0};
        int ok = 1;
        newScan();
        opterr = 0;
        if (getopt(6, argv, "xy:") != 'x') ok = 0;
        if (getopt(6, argv, "xy:") != 'y' || optarg == 0 || strcmp(optarg, "yv") != 0) ok = 0;
        if (getopt(6, argv, "xy:") != -1 || optind != 4) ok = 0;
        if (ok) bits |= 8;
    }
    {
        char p[] = "prog", x[] = "-x", o1[] = "op1", y[] = "-y", yv[] = "yv", o2[] = "op2";
        char *argv[] = {p, x, o1, y, yv, o2, 0};
        int ok = 1;
        newScan();
        opterr = 0;
        if (getopt(6, argv, "xy:") != 'x') ok = 0;
#if defined(_WIN32) || defined(__APPLE__)
        /* POSIX: the first operand ends the scan. */
        if (getopt(6, argv, "xy:") != -1 || optind != 2) ok = 0;
#else
        /* GNU: operands are moved behind the options. */
        if (getopt(6, argv, "xy:") != 'y' || optarg == 0 || strcmp(optarg, "yv") != 0) ok = 0;
        if (getopt(6, argv, "xy:") != -1 || optind != 4 || strcmp(argv[4], "op1") != 0) ok = 0;
#endif
        if (ok) bits |= 16;
    }
    return bits;
}

static int flagScan(void) {
    static int flagValue = 0;
    static struct option const longs[] = {{"set", no_argument, &flagValue, 7}, {0, 0, 0, 0}};
    char p[] = "prog", s[] = "--set";
    char *argv[] = {p, s, 0};
    int idx = -1;
    newScan();
    opterr = 0;
    int const r = getopt_long(2, argv, "", longs, &idx);
    return (r == 0 && flagValue == 7 && idx == 0 && getopt_long(2, argv, "", longs, &idx) == -1)
               ? 32 : 0;
}

int main(void) {
    int const bits = longScan() | shortScans() | flagScan();
    return bits == 63 ? 42 : bits;
}
