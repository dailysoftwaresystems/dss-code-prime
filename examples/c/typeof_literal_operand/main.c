// D-CSUBSET-TYPEOF-VALUE-FORM-RESOLVES-ONLY-FOR-AN-OBJECT-OPERAND (C23 6.7.2.5):
// `typeof(<expression>)` where the operand is NOT a named object.
//
// ⚠ WHY THIS FILE EXISTS AND `examples/c/typeof_basic` DOES NOT COVER IT. That
// example — the witness on which `D-CSUBSET-TYPEOF` was recorded LANDED — writes
// `typeof(base)` with `base` an OBJECT in every one of its positions, and its one
// remaining operand is a TYPE-NAME (`typeof(unsigned short)`). Both sit on the
// WORKING side of a split it could not see. So did every `Typeof*` unit test. The
// broken side is this one: an operand that is a LITERAL, or an expression built
// from one.
//
// ✔MEASURED at 301e2a63 through the shipped CLI on
// x86_64:pe64-x86_64-windows-exec, with gcc 13.3.0 (-std=c2x) and clang 18.1.3
// (-std=c23) probed SEPARATELY — both references compile and run every line
// below, and agree on this exit code:
//   • `typeof(1) x = 3.9;` gave a DOUBLE, with NO diagnostic. `sizeof(x)` was 8
//     where C says 4, and `x * 2` was 7 where C says 6. A wrong answer, not a
//     refusal — the declaration head resolved to nothing, so the Pass-2
//     initializer backfill typed the object from its INITIALIZER instead.
//   • `typeof(uc + 1)` gave `unsigned char` — 1 byte where C 6.3.1.1 integer
//     promotion says 4. The literal half of the operand contributed nothing, so
//     the head took the other half's type. This is the pervasive glibc/kernel
//     idiom, and it was the worse of the two silent answers.
//   • every position that NEEDS a resolved head simply REFUSED: a parameter, a
//     global, an array, a pointer, a typedef, a return type, a struct member.
//
// ★★ THE EXIT CODE DISCRIMINATES THE OPERAND'S TYPE FROM THE INITIALIZER'S, which
// is the whole point and exactly what `typeof_basic` cannot do. Every value below
// is initialized from a `double` whose fractional part is DISCARDED by the
// integer store — so a head that silently adopted the initializer's type would
// keep the fraction and produce a different number, and a head that resolved to
// the wrong integer WIDTH would wrap and produce a different number again.
//
// ⚠ NOTHING HERE CAN BE CONST-FOLDED AWAY. `z` is `argc - 1` — the OS sets argc,
// so it is 0 at run time and unknowable at compile time. Every `double`
// initializer is written `<literal> + z`, which keeps the conversion, the store
// and the arithmetic in the emitted code under the shipped `release` pipeline as
// well as under `debug`.
//
// Data-model-INDEPENDENT: `int` is 4 bytes under LP64 and LLP64 alike, so the one
// exit code holds on all four targets.
//
// exit = v(6) + w(4) + g(6) + t(2) + ap(4) + s.m(9) + c(8) + mm(3) = 42.

static typeof(1) g;                                     // GLOBAL position
typedef typeof(1) T;                                    // TYPEDEF position
struct S { typeof(1) m; };                              // STRUCT-MEMBER position
static typeof(1) twice(typeof(1) p) { return p * 2; }   // RETURN + PARAMETER

int main(int argc, char **argv) {
    (void)argv;
    int z = argc - 1;                                   // 0, decided by the OS

    // (1) THE SILENT MISCOMPILE, both halves at once. A local scalar WITH an
    // initializer is the ONE position that used to "work".
    typeof(1) x = 3.9 + z;          // int 3 — a double x would hold 3.9
    int v = (int)(x * 2);           // 6     — a double x gives 7
    int w = (int)sizeof(x);         // 4     — a double x gives 8

    // (2) GLOBAL — used to fail H_TypeUnresolved.
    g = 6.9 + z;                    // 6

    // (3) TYPEDEF — used to fail S_UnknownType.
    T t = 2.9 + z;                  // 2

    // (4) ARRAY and (5) POINTER — used to fail H_TypeUnresolved.
    typeof(1) a[3] = {1, 2, 3};
    typeof(1) *p = &a[2];
    int ap = a[0] + *p;             // 1 + 3 = 4

    // (6) STRUCT MEMBER — used to fail S_IncompleteTypeObject.
    struct S s;
    s.m = 9.9 + z;                  // 9

    // (7) PARAMETER and RETURN TYPE — used to fail H_TypeUnresolved /
    // S_NotCallable. An int parameter truncates 4.9 to 4 and doubles it; a
    // double parameter would double 4.9 to 9.8 and return 9.
    int c = twice(4.9 + z);         // 8

    // (8) THE MIXED OPERAND — the glibc/kernel idiom. C 6.3.1.1 promotes
    // `uc + 1` to int, so `m` holds 300; an `unsigned char` head would wrap it
    // to 44 and `m / 100` would be 0 instead of 3.
    unsigned char uc = (unsigned char)(250 + z);
    typeof(uc + 1) m = 300.9 + z;   // 300
    int mm = m / 100;               // 3
    (void)uc;

    return v + w + g + t + ap + s.m + c + mm;   // 6+4+6+2+4+9+8+3 = 42
}
