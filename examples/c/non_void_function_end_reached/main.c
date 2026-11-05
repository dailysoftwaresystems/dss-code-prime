/* THE CLOSING BRACE OF A NON-VOID FUNCTION IS REACHED — AND THE PROGRAM IS CONFORMING
 * (D-C-A-NON-VOID-FUNCTION-WHOSE-END-IS-REACHABLE-IS-REFUSED).
 *
 * C23 6.9.2p13: "Unless otherwise specified, if the } that terminates the function
 * body is reached, and the value of the function call is used by the caller, the
 * behavior is undefined." REACHING the brace is therefore defined; only USING the
 * value that was never returned is not. A compiler has to compile the function, return from it, and go on
 * running the caller — gcc, clang, Apple clang, mingw-w64 gcc and MSVC all do,
 * and three of the five warn by default. DSS refused the function outright; it
 * now warns once per function, at the brace, and returns.
 *
 * Each shape changes the exit code when it goes wrong:
 *   some_paths  — `if (k) return 5;` and nothing after it: the end reached twice
 *                 with the value unused, then NOT reached and the value used    (1)
 *   per_case    — a switch over an enumeration with a return in every case and
 *                 no default: a value outside the enumerators reaches the end   (2)
 *   no_return   — no return statement at all                                    (4)
 *   as_pointer, as_double, as_long_double, as_pair, as_wide — other result
 *                 classes, each leaving through its own return convention (the
 *                 last through storage the caller provides): reached with the
 *                 value unused, then returned and compared                      (8)
 *   inl_some_paths, inl_double — the same bodies, which an optimizer may copy
 *                 into `main`: the path that falls off must neither disappear
 *                 nor change what the returning path gives                     (16)
 * `steps` counts the statements that run around each call, so a compiler that
 * took the reached end for unreachable cannot arrive at the right count. Every
 * argument comes from `argc`, so no check folds at compile time.
 */
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

enum colour { RED, GREEN, BLUE };
struct pair { long long first; long long second; };
struct wide { char bytes[64]; };

static volatile int steps;
static int anchor;

NOINLINE int some_paths(int k) {
    steps += 1;
    if (k) return 5;
}

NOINLINE int per_case(enum colour c) {
    steps += 1;
    switch (c) {
    case RED:   return 10;
    case GREEN: return 20;
    case BLUE:  return 30;
    }
}

NOINLINE int no_return(void) {
    steps += 1;
}

NOINLINE void *as_pointer(int k) {
    steps += 1;
    if (k) return &anchor;
}

NOINLINE double as_double(int k) {
    steps += 1;
    if (k) return 2.5;
}

NOINLINE long double as_long_double(int k) {
    steps += 1;
    if (k) return 1.25L;
}

NOINLINE struct pair as_pair(int k) {
    steps += 1;
    if (k) {
        struct pair p = { 11, 22 };
        return p;
    }
}

NOINLINE struct wide as_wide(int k) {
    steps += 1;
    if (k) {
        struct wide w = { { 7 } };
        return w;
    }
}

static int inl_some_paths(int k) {
    steps += 1;
    if (k) return 5;
}

static double inl_double(int k) {
    steps += 1;
    if (k) return 2.5;
}

int main(int argc, char **argv) {
    int one = argc;        /* 1: the program is run with no argument */
    int zero = argc - 1;
    int bad = 0;
    (void)argv;

    some_paths(zero); steps += 10;
    some_paths(zero); steps += 10;
    if (some_paths(one) != 5 || steps != 23) bad |= 1;

    steps = 0;
    per_case((enum colour)(one + 6)); steps += 10;
    if (per_case((enum colour)zero) != 10 || per_case((enum colour)one) != 20
        || per_case((enum colour)(one + 1)) != 30 || steps != 14) bad |= 2;

    steps = 0;
    no_return(); steps += 10;
    no_return();
    if (steps != 12) bad |= 4;

    steps = 0;
    as_pointer(zero); as_double(zero); as_long_double(zero);
    as_pair(zero); as_wide(zero); steps += 10;
    {
        struct pair p = as_pair(one);
        struct wide w = as_wide(one);
        if (as_pointer(one) != &anchor || as_double(one) != 2.5
            || as_long_double(one) != 1.25L || p.first != 11 || p.second != 22
            || w.bytes[0] != 7 || w.bytes[63] != 0 || steps != 20) bad |= 8;
    }

    steps = 0;
    inl_some_paths(zero); inl_double(zero); steps += 10;
    if (inl_some_paths(one) != 5 || inl_double(one) != 2.5 || steps != 14) bad |= 16;

    return bad ? bad : 42;
}
