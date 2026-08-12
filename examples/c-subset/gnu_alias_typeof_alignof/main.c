// D-CSUBSET-TYPEOF-GNU-SPELLING + D-CSUBSET-ALIGNOF-GNU-SPELLING: the GNU
// pre-standard spellings of two type-query operators DSS already implements.
// `__typeof__` / `__typeof` mean exactly what the C23 `typeof` keyword means
// (qualifier-PRESERVING — they are NOT `typeof_unqual`), and `__alignof__` /
// `__alignof` mean exactly what C11 `_Alignof` means. They are keyword-table
// ALIASES onto the EXISTING token kinds; no semantics were added.
//
// WHY THE ALIASES ARE THE SPELLINGS THAT MATTER: `typeof` is C23-only and
// `_Alignof` is C11-only, while the `__`-prefixed forms are available at EVERY
// -std level. A header that must still compile as C89 therefore writes the GNU
// form, which is why real system headers use it. MEASURED on this host with
// matched positive+negative controls — gcc 13.3.0 (-std=gnu17, -std=c2x) and
// clang 18.1.3 / 19.1.1 (-std=c23, -std=gnu17) all accept every spelling below.
//
// This RUNS on every target and the proof is a RUNTIME wrap-at-256, twice, in
// the same shape typeof_basic uses for the ISO spelling: `base` is an
// `unsigned char`, so `__typeof__(base)` is ALSO `unsigned char` and a value
// above 255 stored through it TRUNCATES to 8 bits. If an alias wrongly resolved
// to `int` — or wrongly routed to the qualifier-STRIPPING arm and then to some
// other type — the stores would not wrap and the exit code would differ. `argc`
// is a RUNTIME argument (1 when run with no args), so neither wrap nor the
// cast can be constant-folded away, in the release pipeline either.
//
// Each of the FOUR new spellings is load-bearing here, so deleting ANY ONE of
// its keyword rows from c-subset.lang.json makes this example fail to compile:
//   * `__typeof__`  — w (wrap), su (sizeof of a type-name), castw (cast target)
//   * `__typeof`    — w2 (the second wrap; also proves the lexer takes the
//                     LONGEST match, i.e. `__typeof__` is not read as
//                     `__typeof` followed by a stray `__` identifier)
//   * `__alignof__` — a1
//   * `__alignof`   — a2
//
// All values are data-model-INDEPENDENT (unsigned char / unsigned short widths
// and the alignments of double and int are identical under LP64 and LLP64), so
// the one exit code holds across all four targets AND the release pipeline.
//
// exit = wrapped(4) + wrapped2(3) + a1(8) + a2(4) + su(2) + castw(45) - 24 = 42.
//   (an int-typed `__typeof__(base)` would give wrapped 260 and castw 301 — a
//    different exit, so this is RED on a broken alias, not merely on a missing
//    one.)

int main(int argc, char **argv) {
    (void)argv;

    unsigned char base = (unsigned char)(200 + argc);   // 201 (argc == 1)

    // (1) __typeof__(base) == unsigned char → RUNTIME wrap-at-256 store+load.
    __typeof__(base) w = base + 59;                     // 260 → u8 wrap → 4
    int wrapped = (int)w;                               // 4

    // (2) The second GNU spelling, without the trailing underscores.
    __typeof(base) w2 = base + 58;                      // 259 → u8 wrap → 3
    int wrapped2 = (int)w2;                             // 3

    // (3) __alignof__ of a TYPE-NAME — 8 on every current target.
    int a1 = (int)__alignof__(double);                  // 8

    // (4) __alignof, the single-underscore GNU spelling — 4 everywhere.
    int a2 = (int)__alignof(int);                       // 4

    // (5) sizeof of a __typeof__ TYPE-NAME folds to the underlying's size.
    int su = (int)sizeof(__typeof__(unsigned short));   // 2

    // (6) A cast whose type-name is a __typeof__: 301 re-truncated through
    // unsigned char (301 & 0xFF == 45) and widened back to int.
    int t = 300 + argc;                                 // 301 (argc == 1)
    int castw = (int)(__typeof__(base))t;               // 45

    return wrapped + wrapped2 + a1 + a2 + su + castw - 24;  // 42
}
