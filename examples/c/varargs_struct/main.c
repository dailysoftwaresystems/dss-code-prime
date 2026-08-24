// FC12a-struct (D-FC12A-VARIADIC-CALLEE / D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): the
// first DSS binary that passes a struct BY VALUE across the SysV AMD64 variadic
// boundary AND reads struct varargs via `va_arg(ap, struct ...)`. It exercises:
//
//   * WALL 3 — a FIXED by-value InRegisters struct param `base` in a variadic
//     function. `struct Pt {long a; double b;}` classifies to ONE GPR eightbyte
//     (`a`) + ONE SSE eightbyte (`b`), so receiveByValueParam advances BOTH the GPR
//     and SSE fixed-arg ordinals; va_start then initializes gp_offset/fp_offset PAST
//     the struct's two pieces (gp_offset = (base.a + n) * 8 = 16; fp_offset = 48 +
//     base.b's slot = 64). Reverting wall-3 to fail-loud breaks the build here.
//
//   * THE MIXED-CLASS va_arg GATHER — each `va_arg(ap, struct Pt)` is the ATOMIC
//     register-gather of ONE GPR eightbyte + ONE SSE eightbyte; both cursors must
//     have room, then the GPR eightbyte is copied from reg_save_area+gp_offset and
//     the SSE eightbyte from reg_save_area+fp_offset, bumping EACH cursor by its own
//     stride. (Threading is irrelevant for the {long;double} shape — it has only ONE
//     piece per class — so `sumLL`/`sumDD` below pin the SAME-class threading.)
//
//   * FOLD 2 — THE SAME-CLASS CURSOR THREADING (the #1 silent-miscompile site).
//     `sumLL` reads a `struct LL {long a; long b;}` vararg — TWO GPR eightbytes from
//     CONSECUTIVE save-area slots: piece 0 from reg_save_area+gp_offset, piece 1 from
//     reg_save_area+(gp_offset+8) — the BUMPED cursor. If lowerVaArgAggregate re-read
//     the ORIGINAL cursor for the 2nd piece (the threading regression), `b` would
//     alias `a`'s slot, so `a+b` would become `a+a`. With LL = {100, 7}: correct
//     a+b = 107; the bug yields 100+100 = 200. `sumDD` pins the FPR +16 thread the
//     same way with `struct DD {double a; double b;}` = {3.0, 40.0}: correct
//     (long)a+(long)b = 43; the bug (b aliases a's fp slot) yields 3+3 = 6.
//
//   * THE OPERAND-UNIT variadic payload — `combine(base, 2, v1, v2)` stamps
//     fixedOperandCount in OPERAND units: base (2 register pieces) + the count `2`
//     (1 operand) = 3 fixed operands; the two struct varargs ride beyond that.
//
// All values flow THROUGH the variadic mechanism (they are PASSED as varargs, not
// folded across the call), so the result witnesses the real register-save-area read.
//
//   combine: base = {1, 2.0} -> 3; v1 = {10, 20.0} -> 30; v2 = {5, 7.0} -> 12; sum = 45
//   sumLL:   LL = {100, 7}   -> 100 + 7  = 107        (a+a threading bug -> 200)
//   sumDD:   DD = {3.0, 40.0}-> 3   + 40 = 43         (a+a threading bug -> 6)
//   total = 45 + 107 + 43 = 195  -> exit 195
//
// Runs natively on the x86_64-Linux CI leg.

struct Pt { long a; double b; };
struct LL { long a; long b; };
struct DD { double a; double b; };

long combine(struct Pt base, int n, ...) {
    va_list ap;
    va_start(ap, n);
    long total = base.a + (long)base.b;
    for (int i = 0; i < n; i = i + 1) {
        struct Pt p = va_arg(ap, struct Pt);
        total = total + p.a + (long)p.b;
    }
    va_end(ap);
    return total;
}

// FOLD 2: a 2-GPR struct vararg. Folds a + b so the 2nd GPR piece reading the 1st
// piece's save-area slot (a threading regression) flips a+b into a+a -> wrong exit.
long sumLL(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long total = 0;
    for (int i = 0; i < n; i = i + 1) {
        struct LL p = va_arg(ap, struct LL);
        total = total + p.a + p.b;
    }
    va_end(ap);
    return total;
}

// FOLD 2: a 2-FPR struct vararg. Same threading pin on the SSE (+16) cursor.
long sumDD(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long total = 0;
    for (int i = 0; i < n; i = i + 1) {
        struct DD p = va_arg(ap, struct DD);
        total = total + (long)p.a + (long)p.b;
    }
    va_end(ap);
    return total;
}

int main(void) {
    struct Pt base = {1, 2.0};
    struct Pt v1 = {10, 20.0};
    struct Pt v2 = {5, 7.0};
    long mixed = combine(base, 2, v1, v2);   // 45

    struct LL ll = {100, 7};
    long gpr2 = sumLL(1, ll);                 // 107 (a+a threading bug -> 200)

    struct DD dd = {3.0, 40.0};
    long fpr2 = sumDD(1, dd);                 // 43  (a+a threading bug -> 6)

    return (int)(mixed + gpr2 + fpr2);        // 195
}
