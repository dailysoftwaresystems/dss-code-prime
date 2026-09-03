// D-CSUBSET-VLA C4c: a C99 §6.7.6.2/6.7.6.3 array-PARAMETER declarator with `static`, a
// cv-qualifier, and/or the bare unspecified-size `*` INSIDE the `[ ]` — `int a[static 3]`,
// `int a[static n]`, `int a[const 3]`, `int a[const static 3]`, and `int a[*]`. ALL of them are
// decorations that DECAY the parameter to a plain pointer (C 6.7.6.3p7) with ZERO new codegen:
// each callee below compiles byte-for-byte like its `int *a` twin and RUNS on all 3 legs. The
// decorations carry no runtime effect (a `static N` non-null/at-least-N hint, a cv-qualifier on
// the decayed pointer, a `*` unspecified-size prototype marker); DSS accepts them and drops
// them, exactly as a decayed pointer parameter.
//
// Each callee is exercised by a REAL call with a fixed-array argument and reads through the
// decayed pointer; a distinct return code per failed layer isolates which form regressed, and
// only all-pass -> 42. `sumn`'s `int a[static n]` uses a genuinely runtime `n` (a separate
// parameter) — the `[static n]` decoration decays away, so the sum is driven by the plain `n`,
// proving the runtime form is a plain pointer too.
//
// `int a[*]` (D-CSUBSET-VLA-PARAM-STAR): the bare unspecified-size prototype form — landed via a
// distinct `arrayStarSuffix` grammar rule + the speculative-repeat-alt schema-compiler engine fix
// (grammar_schema_json.cpp) so `[*]` rolls back cleanly to a real bound (`[N]` / the deref-VLA
// `[*p]`). It decays to `int *a` exactly like a bare `[]`; a NON-parameter `[*]` fails loud
// (S_ArrayParamQualifierNonParameter, 0xE054).

// `int a[static 3]`: at-least-3 elements — decays to `int *a`.
int sum(int a[static 3]) { return a[0] + a[1] + a[2]; }

// `int a[static n]` (n a sibling parameter, genuinely runtime): decays to `int *a`; the
// `[static n]` decoration is dropped, so `n` alone drives the read.
int sumn(int n, int a[static n]) {
    int s = 0;
    int i;
    for (i = 0; i < n; i = i + 1) s = s + a[i];
    return s;
}

// `int a[const 3]`: the `const` qualifies the (decayed) pointer — accepted, decays to `int *a`
// (the C4c leniency defers const-ENFORCEMENT on the decayed pointer; never a miscompile — a
// read is always valid).
int firstc(int a[const 3]) { return a[0]; }

// `int a[const static 3]`: the qualifier + static combo — decays to `int *a`.
int sumcs(int a[const static 3]) { return a[0] + a[1] + a[2]; }

// `int a[*]`: the bare unspecified-size PROTOTYPE-form VLA-parameter marker — decays to `int *a`
// and indexes through the decayed pointer exactly like a plain pointer parameter.
//
// ⚠⚠ THIS USED TO BE SPELLED `int gstar(int a[*]) { … }` — ONE declarator, `[*]` ON THE
// DEFINITION — AND THAT PROGRAM IS ILL-FORMED C. The row that shipped it
// (D-CSUBSET-VLA-PARAM-STAR, closed 2026-07-14) taught DSS to PARSE and decay `[*]` and never
// asked WHERE C allows it. C 6.7.6.3p12 permits `[*]` only where the function declarator is
// NOT part of a definition of that function, because a definition's parameters have block
// scope (C 6.2.1p4) and the body needs the bound the `*` withholds.
// ✔MEASURED 2026-09-03, each reference probed SEPARATELY: gcc 13.3.0 refuses (`'[*]' not
// allowed in other than function prototype scope`) and clang 18.1.3 refuses (`variable length
// array must be bound in function definition`); MSVC abstains (no C99 VLA at all, `C2059` on
// the token). So this example's own runtime witness for `[*]` was a program neither working
// reference will compile — ⇒ **a runnable witness proves the feature runs, never that the
// program is legal**, and only probing the references answers the second question.
// It is now spelled the way C intends and the way this feature is actually used: the
// PROTOTYPE carries `[*]`, the DEFINITION spells a real bound. The decay under test is
// unchanged — `gstar` still takes `int *` and still indexes through it.
int gstar(int a[*]);
int gstar(int a[3]) { return a[0] + a[1] + a[2]; }

int main(void) {
    int x[3];
    x[0] = 40;
    x[1] = 1;
    x[2] = 1;

    if (sum(x)     != 42) return 1;   // int a[static 3]
    if (sumn(3, x) != 42) return 2;   // int a[static n]      (runtime n)
    if (firstc(x)  != 40) return 3;   // int a[const 3]
    if (sumcs(x)   != 42) return 4;   // int a[const static 3]
    if (gstar(x)   != 42) return 5;   // int a[*]             (unspecified-size marker)

    return 42;
}
