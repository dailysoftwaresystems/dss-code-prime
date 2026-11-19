/* `sysconf(_SC_NPROCESSORS_ONLN)` — how many processors are online.
 *
 * A `sysconf` name is an index into the C library's own table, so it is a
 * different number on every platform (84 for glibc, 58 for the macOS SDK;
 * ✔MEASURED 2026-10-10 from each platform's own <unistd.h>: gcc 13.3 and
 * clang 18.1 on x86_64 Linux, gcc 13.3 on arm64 Linux, AppleClang 21 on arm64
 * macOS). The shipped <unistd.h> declared the name for ELF alone: this program
 * did not compile for Mach-O, where the platform's header defines it.
 *
 * The wrong number is not a harmless one: the library answers some OTHER
 * question (or -1), so the check below is on the answer, not on the call. The
 * two numbers are also asked for twice, a second time through a variable, so
 * the optimizer has a call it must keep and a value it may fold.
 *
 * Exit 42 = the count is a plausible one and both ways of asking agree.
 */
#include <unistd.h>

static long ask(int name) {
    return sysconf(name);
}

int main(void) {
    long direct = sysconf(_SC_NPROCESSORS_ONLN);
    int name = _SC_NPROCESSORS_ONLN;
    long through = ask(name);
    if (direct < 1 || direct > 65536) {
        return 1;
    }
    if (through != direct) {
        return 2;
    }
    /* A page is at least 4096 bytes on every platform this runs on and the
     * processor count is far below that: an index that named the page size
     * instead would be caught here. */
    if (direct >= sysconf(_SC_PAGESIZE)) {
        return 3;
    }
    return 42;
}
