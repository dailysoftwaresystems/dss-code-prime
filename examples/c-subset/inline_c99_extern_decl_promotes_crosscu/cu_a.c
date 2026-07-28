// TF-C79 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER), CU A — the C99 6.7.4p7
// QUANTIFIER, and the ONE silent halfway state this cycle can have.
//
// 6.7.4p7 makes a definition an inline definition only "if ALL of the file scope
// declarations for a function in a translation unit include the inline function
// specifier without extern". Here one declaration does NOT (the plain `extern`
// prototype), so `payload` gets a real EXTERNAL definition in this TU after all
// — the inline-ness is cancelled by the co-declaration, not by anything written
// on the definition itself.
//
// ★★ THIS IS THE SILENT HALFWAY STATE, AND IT IS WHY THE QUANTIFIER AND THE
// NO-EMIT PATH ARE ONE COMMIT. Get the quantifier wrong — merge the two
// declarations' inline-ness with OR instead of AND, or drop the `extern` test
// from the specifier scan — and this definition is SUPPRESSED instead of
// promoted. The program still compiles, still links, and emits ZERO diagnostics;
// cu_b.c's WEAK fallback body simply wins and the exit code slides 42 → 3. No
// other arm in this cycle fails quietly, which is exactly why it gets its own
// example rather than a line in a bigger one.
//
// The arithmetic makes that slide unmissable: this TU's body returns 40 and
// cu_b's weak fallback returns 1, so a correct build exits 40 + 2 = 42 and the
// broken one exits 1 + 2 = 3.
//
// MEASURED with `/usr/bin/clang -std=c99 -O0 -c` + `nm`: this file yields
// `T _payload` — a strong, defined symbol — for both orderings of the two
// declarations, and the clang-linked pair independently exits 42.

extern int payload(void);

inline int payload(void) { return 40; }

int main(void) { return payload() + 2; }
