// c151 (D-LK1-4 PIE half): the position-independent-executable runtime
// witness — `x86_64:elf64-x86_64-linux-pie` emits ET_DYN + PT_INTERP +
// entry (the modern gcc-default executable shape) and the Linux leg
// RUNS it directly (`./main`) at a KERNEL-RANDOMIZED base.
//
// The body is deliberately the c150 W2 dispatch shape: a const
// function-pointer table (relro -> merged into the writable `.data`
// with one R_X86_64_RELATIVE row per slot) dispatched through at run
// time. Under a slid base the dispatch only lands on pick42 if ld.so
// applied the RELATIVE fixups against the real load base — a broken
// slide computes a garbage pointer and crashes (never a silent wrong
// exit). exitCode 42 = pick42's return.
static int pick40(void) { return 40; }
static int pick42(void) { return 42; }

typedef int (*fn)(void);
static fn const table[2] = { pick40, pick42 };

int main(void) {
    return table[1]();
}
