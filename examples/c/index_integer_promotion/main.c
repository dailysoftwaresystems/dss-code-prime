// D-CSUBSET-INDEX-INTEGER-PROMOTION witness: a `char` subscript of a WIDE-element
// (int) array/pointer integer-PROMOTES to int (C 6.3.1.1 / 6.5.2.1) before the
// index arithmetic. The crux is OVERFLOW: a char index `c=100` of an int array
// (stride 4) needs byte offset 100*4 = 400; computed at CHAR width that wraps
// (400 mod 256 = 144 -> element 36) and reads the WRONG element. Promotion to
// int computes 400 correctly -> a[100]. Both the array-base and pointer-base
// arms + a char-indexed STORE are exercised; signed-char sign-extension too.
//
// RED-ON-DISABLE: revert the cst_to_hir Index integer-promotion arm -> the
// char index stays Char-typed -> scaleIndexToBytes forms a Char-width stride-Mul
// that (a) walls at the sub-native ALU gap (compile fail) OR (b) if it encoded,
// overflows -> a[c] reads element 36 not 100 -> exit 7. Every value rides real
// runtime memory no pass folds (the loop fills the array; the index is a runtime
// char variable).
//
// char-of-char (stride 1) and int-index are UNAFFECTED (no Mul / already >= int).

int main(void) {
    int a[200];
    for (int i = 0; i < 200; i = i + 1) { a[i] = i; }

    char c = 100;                 // overflow-sensitive: 100*4 = 400 > 255
    int load_ok = (a[c] == 100);  // promoted -> a[100]==100; unpromoted -> a[36]==36

    int *p = a;
    char d = 120;                 // char index of a POINTER base, as an lvalue —
                                  // ALSO overflow-sensitive (120*4 = 480 > 255):
                                  // independently witnesses the STORE path under
                                  // overflow (unpromoted -> p[d] writes a[56])
    p[d] = 777;                   // char-indexed STORE
    int store_ok = (p[d] == 777 && a[120] == 777);

    char e = 7;                   // signed char index, low value (sign-ext clean)
    int signed_ok = (a[e] == 7);

    return (load_ok && store_ok && signed_ok) ? 42 : 7;
}
