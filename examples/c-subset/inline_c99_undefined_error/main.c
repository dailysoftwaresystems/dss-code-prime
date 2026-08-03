// TF-C79 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER): a C99 6.7.4p7 inline definition
// that is CALLED but whose external definition exists in no linked translation
// unit must fail LOUD, at the link tier, naming the symbol.
//
// This is the negative half of `inline_c99_inline_definition_crosscu`: same
// source shape, sibling CU removed. Because the inline definition provides no
// external definition, the call has nothing to bind to and the link reports
// K_SymbolUndefined for `missing_body`.
//
// ★ WHY THIS ARM EXISTS AT ALL. "Emits nothing" is only correct if the omission
// is DETECTABLE. Encoding the state as a weak binding, or as a silently dropped
// declaration, would turn this program into either a wrong-body call or a
// mysterious crash. Fail-loud here is what makes the no-emit decision safe to
// make everywhere else — it is the same contract a bare prototype with no
// definition already has.
//
// MEASURED: `/usr/bin/clang -std=c99 -O0` on this exact file also fails to link,
// with `Undefined symbols: _missing_body`, so DSS and the real toolchain agree
// that this program has no external definition to call.

inline int missing_body(int x) { return x + 1; }

int main(void) { return missing_body(41); }
