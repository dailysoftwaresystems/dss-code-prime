// D-CSUBSET-TYPEOF (C23 6.7.2.5): `typeof` / `typeof_unqual` are TYPE-SPECIFIERS
// that resolve to an operand's type — `typeof(x)` is x's type (with qualifiers),
// `typeof_unqual(x)` is x's type with top-level qualifiers stripped, and the
// type-name forms `typeof(T)` / `typeof_unqual(T)` work wherever a type-name goes.
//
// This RUNS on every target and — critically — performs a WRAP-AT-256 store+load
// at RUNTIME through a `typeof(base)`-declared object: `base` is `unsigned char`,
// so `typeof(base) w` is ALSO `unsigned char`, and `200 + argc + 59` (== 260 for
// argc == 1) TRUNCATES to 8 bits (260 & 0xFF == 4). The wrap is the empirical proof
// that a `typeof(expr)` really resolves to the expression's underlying type all the
// way into codegen — a store/load at 8-bit width, then a widen back to int (U8
// ZExt). If `typeof(base)` wrongly resolved to `int`, `w` would hold 260 (no wrap)
// and the exit code would differ — so this is RED on a broken typeof.
//
// `argc` is a RUNTIME argument (the OS sets it — 1 when run with no args), so the
// wrap-at-256 store+load and the cast-to-typeof cannot be constant-folded away even
// in the RELEASE pipeline; the 8-bit truncation really executes.
//
// It also exercises the other forms whose result is exit-witnessed:
//   • `sizeof(typeof(unsigned short))` == 2 — typeof of a TYPE-NAME, folded.
//   • `sizeof(typeof(base))` == 1 — typeof of an EXPRESSION, folded to u8's size.
//   • `typeof_unqual(volatile int) t` — a PLAIN int (the top-level volatile is
//     stripped). The strip has no single-thread VALUE effect, so it is not
//     exit-witnessed here (it is pinned by the TypeofUnqualStripsVolatile unit
//     test); this proves the spelling COMPILES and computes a normal int.
//   • `(typeof(base))t` — a cast whose type-name is a `typeof`, re-truncating
//     through unsigned char and widening back to int.
//
// All values are data-model-INDEPENDENT (unsigned char / unsigned short widths are
// identical under LP64 and LLP64), so the one exit code holds across all four
// targets AND the shipped release pipeline.
//
// exit = wrapped(4) + su(2) + sb(1) + castw(31) + 4 = 42.
//   (an int-typed `typeof(base)` would give wrapped 260 + sb 4 — a different exit,
//    so this is RED on a reverted feature.)

int main(int argc, char **argv) {
    (void)argv;

    // (1) typeof(base) == unsigned char → RUNTIME wrap-at-256 store+load.
    unsigned char base = (unsigned char)(200 + argc);   // 201 (argc == 1)
    typeof(base) w = base + 59;                          // 260 → u8 wrap → 4
    int wrapped = (int)w;                                // 4

    // (2) sizeof of a typeof TYPE-NAME folds to the underlying's size.
    int su = (int)sizeof(typeof(unsigned short));        // 2

    // (3) sizeof of a typeof EXPRESSION folds to u8's size.
    int sb = (int)sizeof(typeof(base));                  // 1

    // (4) typeof_unqual(volatile int) — a PLAIN int (volatile stripped); compute.
    typeof_unqual(volatile int) t = 30 + argc;           // 31 (argc == 1)

    // (5) cast-to-typeof: (typeof(base))t re-truncates 31 through unsigned char
    // (still 31) and widens back to int.
    int castw = (int)(typeof(base))t;                    // 31

    return wrapped + su + sb + castw + 4;                // 4 + 2 + 1 + 31 + 4 = 42
}
