/* D-C-ATOMICS-RUNTIME-PE64-BUS-LOCKS-EVERY-ACCESS-TO-A-CACHE-LINE-STRADDLING-OBJECT
 * — an UNDER-ALIGNED `_Atomic` LOAD FROM READ-ONLY MEMORY must read its value
 * back.
 *
 * ★★★ THE DEFECT THIS CATCHES WAS A CRASH ON A VALID PROGRAM. Reading a `const`
 * `_Atomic` object is ordinary C, and a `static const` object lives in read-only
 * memory. DSS's pe64 atomics runtime used to serve an under-aligned load that did
 * not fit an aligned 8-byte block with a `lock cmpxchg` whose expected and
 * desired values were equal — a "no-op" compare-exchange. It is not a no-op to
 * the memory manager: the instruction WRITES its destination either way, so on
 * a read-only page it faults. ✔MEASURED before the fix, this exact source:
 * exit 0xC0000005 (access violation) on both pe64 arms, baseline and `release`,
 * while clang 18.1.3 with libatomic (-O0 and -O2, the loads reaching
 * `__atomic_load`) and gcc 13.3.0 (-O0 and -O2, inline loads) all returned 42.
 * One working reference makes the behaviour required. After the fix a load never
 * writes: inside one cache line it is a single plain read, and across a line it
 * is a copy made under the process lock.
 *
 * ★★ WHY SIXTY-FOUR RECORDS, AND WHY THE PLACEMENT IS NOT LEFT TO LUCK. Each
 * record is 5 bytes, so member k's `a` sits at byte offset 5k+1 of the table.
 * 5 is coprime with 64, so the 64 offsets take every residue modulo 64 exactly
 * once, WHATEVER address the linker gives the table. That fixes the census by
 * construction: exactly 3 members cross a 64-byte cache line (residues 61, 62,
 * 63), exactly 21 sit inside one at a misaligned `(addr & 7)` of 5, 6 or 7, and
 * the other 40 fit an aligned 8-byte block. All three of the runtime's load arms
 * are therefore reached on every run, and the census below turns a table that
 * stopped reaching them (a changed record size, a changed packing) into a red
 * with its own exit code instead of a quiet green.
 *
 * ★ THE LOADS CANNOT BE FOLDED AWAY. Every access is through an `_Atomic`
 * lvalue whose provable alignment is 1, so it lowers to the atomics runtime at
 * every configuration; ✔MEASURED, the pre-fix `release` arm faulted exactly as
 * the baseline did, which a folded load could not have.
 *
 * exit = 42 when every value reads back; 1 = a wrong value; 2 = a wrong sum;
 *        3 = the placement census moved (the table no longer covers the arms).
 */
#include <stdint.h>

struct __attribute__((packed)) Packed {
    char              c;
    _Atomic unsigned  a;
};

#define R(k) { (char)0x5A, (unsigned)(k) * 0x01010101u }
static const struct Packed g_table[64] = {
    R(0),  R(1),  R(2),  R(3),  R(4),  R(5),  R(6),  R(7),
    R(8),  R(9),  R(10), R(11), R(12), R(13), R(14), R(15),
    R(16), R(17), R(18), R(19), R(20), R(21), R(22), R(23),
    R(24), R(25), R(26), R(27), R(28), R(29), R(30), R(31),
    R(32), R(33), R(34), R(35), R(36), R(37), R(38), R(39),
    R(40), R(41), R(42), R(43), R(44), R(45), R(46), R(47),
    R(48), R(49), R(50), R(51), R(52), R(53), R(54), R(55),
    R(56), R(57), R(58), R(59), R(60), R(61), R(62), R(63),
};

int main(void) {
    int crossesALine = 0, misalignedInsideALine = 0;
    for (int k = 0; k < 64; ++k) {
        uintptr_t const at = (uintptr_t)&g_table[k].a;
        if ((at & 63u) + 4u > 64u) {
            ++crossesALine;
        } else if ((at & 7u) + 4u > 8u) {
            ++misalignedInsideALine;
        }
    }
    if (crossesALine != 3 || misalignedInsideALine != 21) return 3;

    unsigned sum = 0;
    for (int k = 0; k < 64; ++k) {
        unsigned const v = g_table[k].a;
        if (v != (unsigned)k * 0x01010101u) return 1;
        sum += v & 0xFFu;
    }
    return sum == 2016u ? 42 : 2;   /* 0 + 1 + ... + 63 = 2016 */
}
