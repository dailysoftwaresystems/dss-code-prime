// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL): a stack local whose
// EFFECTIVE alignment exceeds one stack slot (`alignas(32)` — 32 > the 16-byte
// slot on every shipped target) needs a dynamically realigned stack pointer, which
// this static frame layout does not build. The compiler must FAIL LOUD at frame
// layout with L_OverAlignedStackLocal (reporting the computed slot bound), NOT
// silently under-align the slot. `alignas(16)` (== slot width) is HONORED via a
// pad; only > slot width reaches this gate.
//
// The local must be address-taken + live so it gets a real frame slot (mem2reg
// cannot promote it to a register, where alignment would be moot).

int sink(int *p);

int main(void) {
    alignas(32) int loc = 7;   // 32-byte alignment > 16-byte slot bound → fail loud
    return sink(&loc);
}
