// FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT, the roundUp tail bump): an
// ODD-SIZE MEMORY-class struct `struct Odd {long a; long b; int c;}` = 20 bytes
// (>16 -> MEMORY class). It witnesses the roundUp(size, gpSlotBytes) tail:
//
//   * THE CALLER COPY rounds the 20-byte aggregate UP to whole 8-byte outgoing slots
//     = ceil(20/8)*8 = 24 bytes (3 slots) — a sub-slot copy that dropped the tail
//     `int c` would flip the exit.
//
//   * THE va_arg(>16B) OVERFLOW BUMP advances overflow_arg_area by roundUp(20,8) = 24
//     (NOT 20, NOT one slot). With TWO struct varargs back-to-back, a wrong tail-round
//     would mis-locate the SECOND struct's slot and read garbage -> wrong exit.
//
// Both structs' fields (incl. the trailing `int c`) are read from the overflow slots
// and summed (anti-folded — passed as varargs), pinning the 24-byte stride across the
// two varargs.
//
//   o1 {1, 2, 3} -> 1+2+3 = 6;  o2 {10, 20, 30} -> 10+20+30 = 60;  total = 66 -> exit 66.
//
// Runs natively on the x86_64-Linux CI leg.

struct Odd { long a; long b; int c; };   // 20B -> MEMORY class (roundUp(20,8)=24)

long combine(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long total = 0;
    for (int i = 0; i < n; i = i + 1) {
        struct Odd s = va_arg(ap, struct Odd);
        total = total + s.a + s.b + (long)s.c;
    }
    va_end(ap);
    return total;
}

int main(void) {
    struct Odd o1 = {1, 2, 3};
    struct Odd o2 = {10, 20, 30};
    return (int)combine(2, o1, o2);          // 6 + 60 = 66
}
