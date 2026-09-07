// P63 [[D-CSUBSET-ATTRIBUTE-MID-DECLARATOR-POSITION-REFUSED]] — the runtime
// witness for the THREE declarator-list attribute positions DSS refused, and
// for the PER-DECLARATOR binding that separates them from the leading one.
//
// Every decorated member follows a `char`, so BOTH its offset AND the aggregate
// size move when the request lands: a parse-and-drop leaves them at 1 and 3.
// THREE declarators everywhere, because two cannot decide the binding — with
// only `a` and `b`, per-declarator and per-declaration give the same numbers.
//
// Reference numbers, ✔MEASURED 2026-09-07, gcc 13.3.0 and clang 18.1.3 probed
// SEPARATELY, build AND run, undecorated control green in every cell.

// (1) AFTER A NON-LAST DECLARATOR — gcc AND clang: a=0 b=1 c=2 sizeof 16.
//     Only `a` is over-aligned; the struct's alignment rises to 16.
struct after_declarator { char a __attribute__((aligned(16))), b, c; };

// (2) AFTER A COMMA — clang: a=0 b=16 c=17 sizeof 32. `c` sits immediately
//     after `b`, which is what makes the binding per-DECLARATOR: the
//     declaration grain would put `c` at 32 and sizeof at 48.
struct after_comma { char a, __attribute__((aligned(16))) b, c; };

// CONTROL, the LEADING position — already shipped, and the one position whose
// binding IS per-declaration. gcc AND clang: a=0 b=16 c=32 sizeof 48.
struct leading { char __attribute__((aligned(16))) a, b, c; };

// CONTROL, the TRAILING position on the LAST declarator — already shipped.
// gcc AND clang: a=0 b=1 c=16 sizeof 32.
struct trailing { char a, b, c __attribute__((aligned(16))); };

// CONTROL, undecorated. gcc AND clang: a=0 b=1 c=2 sizeof 3.
struct plain { char a, b, c; };

// The UNION half — `unionField` is a separate config row, so it is a separate
// omission and needs its own witness.
union after_comma_u { char a, __attribute__((aligned(16))) b, c; };

// (3) FILE SCOPE, AFTER A COMMA — gcc, clang AND mingw-w64 gcc all build and
//     run this; DSS refused it with `P_NoAlternativeMatched`. `fc` must sit
//     immediately after `fb`, proving the attribute did not leak rightwards.
char fa, __attribute__((aligned(16))) fb, fc;

// CONTROL, file scope, after a declarator — DSS already got this right.
char ga __attribute__((aligned(16))), gb, gc;

// Statically allocated instances, so the alignment is asserted at the SINK
// rather than only in the type.
static struct after_declarator s1;
static struct after_comma     s2;
static struct leading         s3;
static struct trailing        s4;
static union  after_comma_u   s5;

int main(void) {
    // (1) after a non-last declarator: only `a` moves, sizeof rises to 16.
    if (__builtin_offsetof(struct after_declarator, a) != 0)  return 50;
    if (__builtin_offsetof(struct after_declarator, b) != 1)  return 51;
    if (__builtin_offsetof(struct after_declarator, c) != 2)  return 52;
    if (sizeof(struct after_declarator) != 16)                return 53;
    if (((unsigned long long)&s1.a) % 16 != 0)                return 54;

    // (2) after a comma: `b` moves to 16 and `c` follows it at 17 — NOT 32.
    if (__builtin_offsetof(struct after_comma, a) != 0)       return 55;
    if (__builtin_offsetof(struct after_comma, b) != 16)      return 56;
    if (__builtin_offsetof(struct after_comma, c) != 17)      return 57;
    if (sizeof(struct after_comma) != 32)                     return 58;
    if (((unsigned long long)&s2.b) % 16 != 0)                return 59;

    // CONTROL leading: per-DECLARATION — all three move.
    if (__builtin_offsetof(struct leading, a) != 0)           return 60;
    if (__builtin_offsetof(struct leading, b) != 16)          return 61;
    if (__builtin_offsetof(struct leading, c) != 32)          return 62;
    if (sizeof(struct leading) != 48)                         return 63;
    if (((unsigned long long)&s3.b) % 16 != 0)                return 64;

    // CONTROL trailing on the last declarator.
    if (__builtin_offsetof(struct trailing, a) != 0)          return 65;
    if (__builtin_offsetof(struct trailing, b) != 1)          return 66;
    if (__builtin_offsetof(struct trailing, c) != 16)         return 67;
    if (sizeof(struct trailing) != 32)                        return 68;
    if (((unsigned long long)&s4.c) % 16 != 0)                return 69;

    // CONTROL undecorated — the numbers the decorated cases must differ from.
    if (__builtin_offsetof(struct plain, b) != 1)             return 70;
    if (__builtin_offsetof(struct plain, c) != 2)             return 71;
    if (sizeof(struct plain) != 3)                            return 72;

    // The union half.
    if (sizeof(union after_comma_u) != 16)                    return 73;
    if (((unsigned long long)&s5.b) % 16 != 0)                return 74;

    // (3) file scope after a comma: `fb` aligned, `fc` immediately after it.
    if (((unsigned long long)&fb) % 16 != 0)                  return 75;
    if ((unsigned long long)&fc != (unsigned long long)&fb + 1) return 76;

    // CONTROL file scope after a declarator: `ga` aligned, `gb` right after.
    if (((unsigned long long)&ga) % 16 != 0)                  return 77;
    if ((unsigned long long)&gb != (unsigned long long)&ga + 1) return 78;

    // Every decorated object is writable and readable at its aligned address —
    // a placement that merely LOOKS aligned but overlaps a neighbour would be
    // caught here rather than in the arithmetic above.
    s1.a = 1; s2.b = 2; s3.b = 3; s4.c = 4; s5.b = 5; fb = 6; ga = 7;
    if (s1.a + s2.b + s3.b + s4.c + s5.b + fb + ga != 28)     return 79;

    return 42;
}
