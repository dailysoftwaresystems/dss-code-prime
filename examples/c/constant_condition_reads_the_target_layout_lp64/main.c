/* A CONDITION THAT IS A CONSTANT ONLY THROUGH THE TARGET'S LAYOUT — AND WHOSE VALUE
 * DIFFERS BETWEEN TARGETS (D-C-A-NON-VOID-FUNCTION-WHOSE-END-IS-REACHABLE-IS-REFUSED).
 *
 * `sizeof(long)` is 8 where the data model is LP64 (the ELF and Mach-O pairs) and 4
 * where it is LLP64 (the Windows pair). So of the two functions below exactly ONE
 * ends in an `if` that always returns — its closing brace is never reached and
 * nothing may be said about it — and the OTHER ends in an `if` that never fires:
 * its end is reached on every call and it is reported, once, at its brace. Which
 * is which depends on the TARGET the program is compiled for, never on the host
 * the compiler runs on: a compiler that folded `sizeof` with its own host's sizes,
 * or did not fold it at all, gets one of the two manifests wrong.
 *
 * THIS FILE IS ONE OF A PAIR WITH IDENTICAL BYTES:
 *   examples/c/constant_condition_reads_the_target_layout_lp64   (this text, LP64 targets)
 *   examples/c/constant_condition_reads_the_target_layout_llp64  (this text, the LLP64 target)
 * Each manifest states the ONE warning its targets give, by line — and the key is
 * exact over the code it declares, so the other function's silence is asserted too.
 * Change one copy and the other must change with it.
 *
 * The program exits 42 on every target: it uses the value of whichever function
 * returns one there, and only calls the other (a call that ends at the closing
 * brace returns; using its value is what would be undefined — C23 6.9.2p13).
 */
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

static volatile int calls;

NOINLINE int when_long_is_eight(int x) {
    calls += 1;
    if (sizeof(long) == 8) return x;
}

NOINLINE int when_long_is_not_eight(int x) {
    calls += 1;
    if (sizeof(long) != 8) return x;
}

int main(int argc, char **argv) {
    int value;
    (void)argv;
    if (sizeof(long) == 8) {
        when_long_is_not_eight(argc);
        value = when_long_is_eight(argc + 39);
    } else {
        when_long_is_eight(argc);
        value = when_long_is_not_eight(argc + 39);
    }
    return value + calls;
}
