/* P69 fold H5 (lane hm): a mingw-w64 gcc program whose `main` names what gcc takes from its own static runtime,
 * which DSS's PE `compilerSupport` runtime defines (tests/link/test_pe_compiler_support_runtime.cpp):
 *   main                   gcc's `main` calls __main first
 *   gr_big                 a frame past a page                     -> ___chkstk_ms
 *   gr_inner (-fexceptions) a local with a `cleanup`                -> __gcc_personality_seh0 names the frame's
 *                                                                     handler, its landing pad calls _Unwind_Resume
 *   gr_outer               mingw-w64's __try1/__except1: a __C_specific_handler scope with a filter, above gr_inner
 * What it returns: 42 when the normal path ran the cleanup once and then the code after the scope (C H), a FOREIGN
 * exception (RaiseException) reached the filter and then the scope's landing point without running the cleanup (F H,
 * as libgcc does), and the big frame computed its value. With an argument `g` it raises an exception in libgcc's own
 * protocol (the throw code) through the cleanup frame instead, and prints nothing: the personality's answer to that
 * is the measurement.
 *
 * Built through probe-reference-cc's keep mode (object mode, each twice, the two reports' md5s equal), the probe's own
 * directory mapped away so the object records no host path:
 *   gcc -O0 -fexceptions -ffile-prefix-map={probe}=.                     -> ..._gcc_O0_fexceptions.o
 *   gcc -O2 -fexceptions -ffile-prefix-map={probe}=.                     -> ..._gcc_O2_fexceptions.o
 * (MinGW-W64 gcc 13.2.0, x86_64-ucrt-posix-seh; the object's only name of its source is `probe.c`, the probe's.)
 * The same source in run mode is gcc's own measurement (its link, its libgcc): 42 at -O0 and at -O2, `gr_filter`
 * being extern so -O2 keeps it. */
#include <excpt.h>
#include <windows.h>

static char order[16];
static int  at;
static void note(char c) {
    if (at < 15) order[at++] = c;
}

static void unlock(int *p) {
    (void)p;
    note('C');
}

static volatile unsigned long raised;

__attribute__((noinline)) int gr_inner(int raise) {
    int x __attribute__((cleanup(unlock))) = 1;
    if (raise) RaiseException(raised, 0, 0, NULL);
    return x;
}

long gr_filter(EXCEPTION_POINTERS *ep, void *frame) {
    (void)frame;
    note('F');
    return ep->ExceptionRecord->ExceptionCode == 0xE0000001u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static volatile int sink;

__attribute__((noinline)) void gr_outer(int raise) {
    __try1(gr_filter)
    sink = gr_inner(raise);
    __except1
    note('H');
}

__attribute__((noinline)) int gr_big(int seed) {
    volatile char buf[70000];
    for (int i = 0; i < 70000; i += 4096) buf[i] = (char)seed;
    buf[69999] = (char)seed;
    return buf[0] + buf[69999];
}

static int same(char const *a, char const *b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == 'g') {
        raised = 0x20474343u;
        gr_outer(1);
        return 7;
    }
    raised = 0xE0000001u;
    gr_outer(0);
    order[at] = 0;
    int normal = same(order, "CH");
    at         = 0;
    gr_outer(1);
    order[at] = 0;
    int foreign = same(order, "FH");
    return normal && foreign && gr_big(3) == 6 ? 42 : (normal ? 0 : 10) + (foreign ? 0 : 20) + 1;
}
