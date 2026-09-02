// D-CSUBSET-POINTEE-CONST-ENFORCEMENT witness — THE UNION BOUNDARY (P48 lane cq).
//
// Every other shape this row closed is a UNANIMOUS hard reject and DSS errors on
// it. This one is not, and it is the reason this example exists as a RUNNABLE
// program rather than as another expected diagnostic: a manifest can assert
// WHICH diagnostic was produced, but only an executing artifact can assert that
// the program was still BUILT.
//
// ✔MEASURED 2026-09-01, each reference probed SEPARATELY at -O0 AND -O2:
//   gcc 13.3.0 (-std=c2x)        WARNING "assignment of read-only location
//                                's.v'", exit 0 — and the built program returns
//                                42 at BOTH -O0 and -O2, so the write is really
//                                performed
//   mingw-w64 gcc 13.2.0         the same warning, exit 0
//   clang 18.1.3 (-std=c23)      ERROR, refuses
//   MSVC 19.51.36252 (/std:c17)  error C2166, refuses
// gcc's verdict is unchanged under `-std=c17`, `-Wall -Wextra` and even
// `-pedantic-errors`, and gcc ERRORS on the identical write to a NON-bit-field
// const member — so this is a narrow, deliberate gcc leniency, not an artifact
// of how the probe was written.
//
// `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C`, and the references here split on
// ACCEPT-vs-REFUSE rather than on what the program MEANS, so the disjunction
// governs: an Error would be a refusal BELOW the union. Silence is equally
// wrong — C 6.3.2.1p1 excludes a const-qualified type from the modifiable
// lvalues C 6.5.16.1p1 requires, so a diagnostic is owed and all four
// references emit one. DSS therefore WARNS and COMPILES, the same resolution
// this repo reached for S_UnknownAttribute; `--warnings-as-errors` gives anyone
// who wants clang's posture exactly that.
//
// ⚠ THE PREDICATE IS THE MEMBER'S OWN `const`, NOT BIT-FIELD-NESS. A PLAIN
// bit-field of a CONST OBJECT is a hard error on all four references and stays
// an Error in DSS — pinned by
// SemanticAnalyzerC.WriteToAPlainBitFieldOfAConstObjectIsStillAnError.
//
// RED-ON-DISABLE, both directions: drop `constMarker` from c.lang.json's
// `structField` row and the S_ConstViolation disappears entirely (no diagnostic
// at all, the pre-P48 silence); make the bit-field arm of `ConstLvalueVerdict`
// unconditional Error instead and this example FAILS TO BUILD, so the exit-42
// assertion below cannot be reached.

struct Slots {
    const int locked : 8;   // written below: warns on gcc/DSS, refused by clang
    int       open   : 8;
};

int main(void) {
    struct Slots s = {0, 0};
    s.locked = 34;          // the const-declared bit-field write
    s.open   = 8;           // its plain twin, which must draw nothing at all
    return s.locked + s.open;
}
