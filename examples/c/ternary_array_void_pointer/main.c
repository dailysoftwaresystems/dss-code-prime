// D-CSUBSET-TERNARY-ARRAY-ARM-INCOMPATIBLE part (2) witness (positive / runtime).
//
// C 6.5.15p6 lists six admissible operand pairings for a conditional. The last
// one — "one operand is a pointer to an object type and the other is a pointer
// to a qualified or unqualified version of void" — was the one DSS did not
// implement, and it REFUSED the construct:
//   error[H_UnsupportedLoweringForKind]: lvalue kind 'Cast' (ordinal 32)
// because neither the literal-0 arm (the `0` is not bare) nor the array-decay
// arm (a `char` element is not a `void` one) fired, so the conditional typed as
// the ARRAY and reached the aggregate-Ternary lowering.
//
// ✔MEASURED 2026-09-01, each reference probed SEPARATELY at -O0 AND -O2:
// gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51.36252 ALL
// compile `c ? "%s" : (void *)0` with NO diagnostic at all. So DSS's refusal sat
// BELOW `DSS = (gcc u clang u MSVC) u ISO C`, and the fix is ACCEPTANCE.
//
// The exit code is load-bearing rather than decorative: each arm is SELECTED at
// runtime and the selected pointer is DEREFERENCED / compared, so a conditional
// that produced the wrong arm, or a null where the string belongs, changes the
// answer instead of merely still compiling.
//
// RED-ON-DISABLE: revert the `void *` arm in the semantic `combineTernary` and
// in the cst_to_hir `combineTernary` -> the conditional types as `char[3]` again
// and the compile fails H_UnsupportedLoweringForKind (it does not merely produce
// a different number).

static const char *pick(int c) { return c ? "%s" : (void *)0; }

// The reversed arm order, and the plain object-pointer form of the same pairing.
static void *pick_ptr(int c, char *p, void *v) { return c ? v : p; }

int main(void) {
    char        buf[4] = {'A', 'B', 'C', 0};
    const char *a      = pick(1);      // the string arm
    const char *b      = pick(0);      // the void*/null arm
    if (a == 0) return 1;
    if (a[0] != '%') return 2;         // the SELECTED arm really is the literal
    if (a[1] != 's') return 3;
    if (b != 0) return 4;              // the void* arm really is the null
    char *r = (char *)pick_ptr(0, buf, buf + 2);
    if (r != buf) return 5;            // else-arm order: object pointer selected
    char *w = (char *)pick_ptr(1, buf, buf + 2);
    if (w != buf + 2) return 6;        // then-arm order: void* selected
    if (*w != 'C') return 7;
    return 42;
}
