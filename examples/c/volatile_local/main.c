// c21 (D-CSUBSET-VOLATILE-QUALIFIER) — the volatile LOCAL-object pin.
//
// `x` is a `volatile int` local. Mem2Reg refuses to promote a volatile alloca
// (mem2reg.cpp), so its Store and each Load stay real memory operations rather
// than collapsing to SSA values. This corpus drives a volatile local all the way
// through the RELEASE optimizer pipeline (Mem2Reg/CSE/DCE) and runs it: it pins
// that a volatile local lowers + optimizes + executes correctly (no crash, no
// gross miscompile) — the end-to-end companion to the MIR-tier red-on-disable
// test that asserts the volatile Load survives Mem2Reg. The arithmetic is chosen
// so the program returns 42 (a re-read of x after a re-assign must observe 7).
int main(void) {
    volatile int x = 5;
    int a = x;        // volatile load (5)
    x = 7;            // volatile store
    int b = x;        // volatile load — must observe 7, not a cached 5
    return a + b + 30; // 5 + 7 + 30 == 42
}
