// P55 D-SEMANTIC-DEPTH-CAPS-TRUNCATE-INTO-TWO-WRONG-ANSWERS (negative): the C23
// 6.7.6.1p2 diagnostic must survive DEPTH. Two declarations of one function that
// differ ONLY in a pointee `const` are incompatible — 6.7.6.1p2 makes two pointer
// types compatible only when the types they point to are "identically qualified"
// — and until P55 the qualifier walk that answers this stopped at two hard-coded
// depths and returned "no claim", so the diagnostic simply vanished and the
// ill-formed program compiled rc=0 with nothing said.
//
// Both shapes here sit PAST the caps that were removed:
//   * `nested_fn` — FIVE levels of nested function-pointer parameters (the old
//     `nestDepth >= 4`); ✔MEASURED before P55: diagnosed at four levels, SILENT
//     at five.
//   * `deep_group` — SEVENTEEN nested parenthesized declarators (the old
//     `depth > 16`); ✔MEASURED before P55: diagnosed at sixteen, SILENT at
//     seventeen. ISO C23 5.2.4.1 obliges an implementation to support 63, so the
//     old cap was below the standard's own floor as well.
//
// ✔THE VERDICT IS UNANIMOUS, each reference probed SEPARATELY on these exact two
// pairs: gcc 13.3.0 (`-std=c2x -pedantic-errors`), clang 18.1.3 (`-std=c23
// -pedantic-errors`) and mingw-w64 gcc 13.2.0 all report `conflicting types for
// 'h'` at five levels, at seventeen parens and at 100 parens; MSVC 19.51.36252
// (`/std:clatest`) reports `warning C4028: formal parameter 1 different from
// declaration`. Every reference diagnoses; DSS said nothing.
//
// RED-ON-DISABLE: restore either cap (`nestDepth >= 4` in the nested-parameter
// arm, or `depth > 16` in the group-chain descent of `declaratorConstSpine`) and
// the matching declaration below stops being reported — the manifest asserts the
// WHOLE diagnostic set, so losing either one is red. The shallow twins in
// `deep_declarator_qualifier_spine_runs` are the over-refusal control: they are
// legal and must stay accepted.
void nested_fn(void (*)(void (*)(void (*)(void (*)(void (*)(const char *))))));
void nested_fn(void (*)(void (*)(void (*)(void (*)(void (*)(char *))))));

void deep_group(const char *(((((((((((((((((*))))))))))))))))));
void deep_group(char *(((((((((((((((((*))))))))))))))))));

int main(void) { return 0; }
