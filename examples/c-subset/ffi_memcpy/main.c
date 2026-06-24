// D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD regression (the `ffi_memcpy` probe):
// calling the shipped <string.h> `memcpy` — a MULTI-param FnSig (void*, const
// void*, size_t) — with address-of args used to dangle a retained fnParams() span
// in checkCallAgainstSig (the `&b` arg materializes pointer<int> mid-loop, mutating
// the interner pool). It was a heap-use-after-free MASKED in Release (returns 42 by
// luck) and caught only on Debug (the guard aborts the compile, rc=3). The fix
// copies the param span into an owned vector before the per-arg loop.
//
// RED-ON-DISABLE: on a DEBUG-built compiler, reverting the fix makes THIS example
// fail to compile (the guard aborts). b := a (42) via memcpy → exit 42.
#include <string.h>
int main() {
    int a;
    int b;
    a = 42;
    b = 7;
    memcpy(&b, &a, 4);
    return b;
}
