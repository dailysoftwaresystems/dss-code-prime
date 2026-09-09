// The SECOND translation unit, and it is what makes this example test MEANING
// rather than acceptance. `main.c` reads `g_ext` through a plain
// `extern int g_ext;` — an IMPORT — so the only way the program links is if THIS
// declaration DEFINED the object with EXTERNAL linkage.
//
// C 6.9.3p1: a file-scope declaration WITH an initializer is a DEFINITION, and
// C23 6.7.2p15 says `auto` "is IGNORED for the purposes of determining a storage
// duration or linkage" when it appears beside another storage-class specifier —
// it only says the type may be inferred. So `extern` decides the linkage alone
// and the initializer decides the definedness: `extern auto g_ext = 11;` is an
// EXTERNALLY-LINKED DEFINITION of an `int`, and the `extern` is redundant rather
// than contradictory.
//
// ✔MEASURED 2026-09-08, each reference probed SEPARATELY on its own translation
// unit: gcc 13.3.0 `-std=c2x -c` rc=0 and clang 18.1.3 `-std=c23 -c` rc=0, `nm`
// reading `D g_ext` on BOTH (upper case D — DEFINED, external). The whole
// two-file program below builds and runs to exit 42 on both.
//
// ⚠ IF DSS MIS-LOWERED THIS AS AN ExternGlobal IMPORT the failure would be a
// LINK error naming an undefined symbol, not a wrong answer — which is the
// point of splitting the definition into its own TU rather than reading it in
// the same file where a stale in-TU symbol could satisfy the reference.

extern auto g_ext = 11;
