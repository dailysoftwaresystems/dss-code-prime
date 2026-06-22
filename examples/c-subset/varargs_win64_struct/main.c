// FC12b (D-FC12B-WIN64-STRUCT-VARARG): the FIRST DSS binary that passes structs
// BY VALUE across the Win64 (ms_x64) variadic boundary AND reads them via
// `va_arg(ap, struct ...)`, run on THIS Windows host as a PE executable. It pins
// the homogeneous-pointer struct-vararg path for BOTH Win64 by-value shapes:
//
//   * A pow2-≤8B struct (`struct Small {int x; int y}` = 8B) → InRegisters[1 GPR
//     piece]: the caller writes the struct's 8 bytes into ONE arg slot BY VALUE
//     (appendByValueArg loads it as one I64), and `va_arg(struct Small)` returns
//     the SLOT'S ADDRESS directly — the slot IS the storage (no deref). The fields
//     are byte-copied out of the slot. A wrong by-value placement or a spurious
//     deref reads garbage → wrong total.
//
//   * A NON-pow2/>8B struct (`struct Big {long a,b,c}` = 24B) → ByReference: the
//     caller copies the struct to a private temp and passes a HIDDEN POINTER to it
//     in ONE arg slot; `va_arg(struct Big)` DEREFERENCES the slot to recover the
//     copy's address, then reads the fields. A missing deref (returning the slot
//     addr = a pointer-to-pointer) → the consumer copies pointer+garbage → wrong
//     total. The cursor still bumps by ONE slot (the slot holds a pointer, NOT the
//     24 bytes) — a SysV-style roundUp(24,8) bump would desync the next vararg.
//
// Win64 has NO register/stack split and NO SysV MEMORY-class-to-stack rule: each
// struct vararg is EXACTLY ONE arg slot (value or pointer), placed register-or-
// stack BY POSITION like a scalar. `combine(int tag, ...)` takes the two structs as
// varargs; under Win64 the first 4 args occupy the contiguous 32-byte HOME space
// (tag in the rcx slot, Small's value in the rdx slot, Big's pointer in the r8
// slot) — the va_arg walk reads them back from there.
//
// ANTI-FOLD: every struct field derives from a MUTABLE GLOBAL `g_seed` loaded at
// runtime — opaque to ConstFold/Mem2Reg, so the call's arg marshaling (the by-value
// copy + the hidden-pointer copy) and the va_arg walk run at runtime in BOTH the
// baseline and the optimizedPipelines arm; the values can't collapse to a literal
// exit.
//
// EXIT ARITHMETIC (g_seed = 5):
//   tag   = g_seed                              = 5
//   Small = { g_seed + 1, g_seed + 2 }          = { 6, 7 }   (by VALUE in one slot)
//   Big   = { g_seed + 3, g_seed + 4, g_seed+5 }= { 8, 9, 10 } (by POINTER in one slot)
//   total = tag + s.x + s.y + b.a + b.b + b.c
//         = 5 + 6 + 7 + 8 + 9 + 10             = 45  -> exit 45
// A broken by-value slot, a missing/spurious deref, or a desynced cursor flips the
// total off 45. Runs on the Windows host's differential ctest.

struct Small { int  x; int  y; };            // 8B  -> InRegisters[1], by value
struct Big   { long a; long b; long c; };    // 24B -> ByReference, hidden pointer

// Mutable global seed — its runtime load is opaque to ConstFold/Mem2Reg.
int g_seed = 5;

long combine(int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    struct Small s = va_arg(ap, struct Small);   // ≤8B by-value slot (slot IS storage)
    struct Big   b = va_arg(ap, struct Big);     // >8B by-reference slot (deref)
    va_end(ap);
    return (long)tag + s.x + s.y + b.a + b.b + b.c;
}

int main(void) {
    int g = g_seed;                              // runtime-opaque
    struct Small s = { g + 1, g + 2 };           // {6, 7}
    struct Big   b = { g + 3, g + 4, g + 5 };    // {8, 9, 10}
    return (int)combine(g, s, b);                // 45
}
