/* Where the stack stands in DSS code, three frames down from the process entry (P69,
 * D-LK-PROCESS-ENTRY-BIAS-TAKEN-FROM-THE-CALLING-CONVENTION). Every exec format's ABI promises a function 16-byte
 * stack alignment at each call, and DSS lays a frame out trusting it: a `_Alignas(16)` local sits at a 16-aligned
 * frame offset, so its ADDRESS is 16-aligned exactly when the stack is. Whether it is depends on the entry trampoline
 * starting from the right place — and where the stack stands at the trampoline's first instruction depends on how the
 * platform's loader entered the image: CALLED (dyld for Mach-O, BaseThreadInitThunk for pe — a return address pushed)
 * or JUMPED TO (the ELF kernel / ld.so — nothing pushed). Each exec format declares its `entryTransition`; the
 * trampoline derives its bias from it. Before P69 the bias came from the calling convention, and the one `sysv_amd64`
 * convention gave Mach-O x86_64 ELF's answer: every DSS frame there ran 8 bytes off (main=8). The address passes
 * through a volatile integer before `% 16`, so no optimizer can fold it from the declared alignment. */
#include <stdint.h>
#include <stdio.h>

static int mod16(void volatile *p) {
    uintptr_t volatile address = (uintptr_t)p;
    return (int)(address % 16);
}

static int leaf(void) {
    _Alignas(16) volatile char probe[16];
    probe[0] = 1;
    return mod16(probe);
}

static int middle(void) {
    _Alignas(16) volatile char probe[16];
    probe[0] = 1;
    return mod16(probe) * 10 + leaf();
}

int main(void) {
    _Alignas(16) volatile char probe[16];
    probe[0] = 1;
    int const here  = mod16(probe);
    int const below = middle();
    printf("main=%d middle=%d leaf=%d\n", here, below / 10, below % 10);
    return here == 0 && below == 0 ? 42 : 1;
}
