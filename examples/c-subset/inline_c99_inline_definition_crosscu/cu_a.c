// TF-C79 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER), CU A — the C99 6.7.4p7 INLINE
// DEFINITION, and the sharpest observable form of "provides no external
// definition".
//
// Every file-scope declaration of `pick` in THIS translation unit spells
// `inline` without `extern`, so 6.7.4p7 makes this an INLINE definition: it
// "does not provide an external definition for the function". This TU therefore
// emits NO body for `pick`, and the call in `main` resolves against the EXTERNAL
// definition in cu_b.c.
//
// ★ WHY THE EXIT CODE IS THE WITNESS AND NOT A SYMBOL DUMP. The two bodies
// return DIFFERENT values (7 here, 42 there), so the exit code names WHICH body
// ran — it is not merely "the program linked". Exit 42 can only mean cu_b's
// body executed, which can only happen if this TU emitted nothing for `pick`.
//
// RED-ON-DISABLE, two ways, both loud: emit this body and the link fails
// K_SymbolRedefinedAcrossUnits (two external definitions of `pick`); suppress it
// AND fail to leave the ExternFunction declaration behind and the reference has
// nothing to bind to.
//
// MEASURED with `/usr/bin/clang -std=c99 -O0 -c` + `nm`: this file alone yields
// `U _p`-shaped output for `pick` — the symbol is UNDEFINED, not defined-and-
// weak — and the clang-linked pair independently exits 42.

inline int pick(void) { return 7; }

int main(void) { return pick(); }
