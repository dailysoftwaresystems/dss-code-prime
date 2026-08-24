// D-LANG-DIRECT-CALL-INT-POINTEE-COMPAT — the RUNTIME witness that a direct call
// argument whose pointee is an integer of the SAME representation but a DIFFERENT
// identity is not merely ADMITTED but actually WRITES THROUGH correctly.
//
// WHY THIS EXISTS. TF-C41 admitted this shape only when the callee came from a
// shipped FFI descriptor. A real header hits it too: on Darwin/LP64 `tcl.h` defines
// TCL_WIDE_INT_IS_LONG and so declares `Tcl_WideInt` as `long`, while
// `sqlite3_int64` is `long long` — so upstream sqlite's
// `Tcl_GetWideIntFromObj(interp, objv[4], &iVal)` passes `long long*` into a `long*`
// parameter on macOS and nowhere else. Apple clang 21.0.0 compiles that with one
// `-Wincompatible-pointer-types` warning (MEASURED, all four macOS SDKs); DSS
// hard-errored, and both mach-o legs' testfixtures died on it.
//
// ★ THE PAIR IS CHOSEN PER DATA MODEL ON PURPOSE, SO EVERY LEG EXERCISES THE
// FEATURE RATHER THAN SKIPPING IT. No single pair of C integer types is
// same-representation everywhere: `long`/`long long` collide only on LP64, and
// `int`/`long` only on LLP64. Writing the example for one of them would leave the
// other data model compiling a DIFFERENT, uninteresting program while still
// reporting green — the "the test ran but exercised nothing" failure. `__LP64__` is
// declared by the OBJECT FORMAT (every LP64 *.format.json), so this selects on the
// data model, never on an OS or a CPU.
//
// ★ THE VALUE MUST TRAVEL THROUGH THE POINTER. The admission realizes a Ptr→Ptr
// bitcast, which lowers to nothing; that is precisely why "it compiled" proves so
// little here. Each shape below has the CALLEE do the store and `main` read it back,
// so a bitcast that pointed somewhere else, or a store of the wrong width, changes
// the exit code instead of passing quietly.
//
// exitCode 42 = every shape agreed. Any other exit code is a BITMASK naming which
// shape broke (1 = scalar write-through, 2 = the array element, 4 = the
// read-modify-write, 8 = the negative value / sign preservation), so a failure says
// WHICH shape failed rather than reporting an anonymous wrong number.

#ifdef __LP64__
// LP64 (elf64 x86_64/arm64, macho64 x86_64/arm64): `long` and `long long` are both
// 64-bit — the sqlite-on-Darwin pair, byte for byte.
typedef long      callee_int_t;
typedef long long caller_int_t;
#else
// LLP64 (pe64): `long` is 32-bit, so it collides with `int` instead.
typedef long callee_int_t;
typedef int  caller_int_t;
#endif

// Ordinary C prototypes — NOT a shipped FFI descriptor. That is the whole point:
// what decides is the pointee types, not where the declaration came from.
static void store_value(callee_int_t *p, callee_int_t v) { *p = v; }
static void bump_value(callee_int_t *p)                  { *p = *p + 1; }

int main(void) {
    int result = 0;

    // 1 — the plain write-through: the callee stores, the caller reads it back
    //     through a pointer of the OTHER identity.
    caller_int_t scalar = 0;
    store_value(&scalar, 7);
    if (scalar == 7) result |= 1;

    // 2 — an ARRAY ELEMENT's address, so the argument is a computed lvalue rather
    //     than a bare object address (a bitcast that lost the index would show up
    //     here and nowhere above).
    caller_int_t arr[3];
    arr[0] = 0; arr[1] = 0; arr[2] = 0;
    store_value(&arr[2], 11);
    if (arr[2] == 11 && arr[0] == 0 && arr[1] == 0) result |= 2;

    // 3 — read-modify-write THROUGH the converted pointer: the callee must see the
    //     value the caller wrote, not a fresh or stale slot.
    caller_int_t rmw = 41;
    bump_value(&rmw);
    if (rmw == 42) result |= 4;

    // 4 — a NEGATIVE value, so a wrong-width or wrong-signedness store cannot pass
    //     by accident the way a small positive one can.
    caller_int_t negative = 0;
    store_value(&negative, -1234);
    if (negative == -1234) result |= 8;

    // 1|2|4|8 = 15 → 42 only when every shape agreed.
    return result == 15 ? 42 : result;
}
