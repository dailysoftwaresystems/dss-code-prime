// FC17 (D-CSUBSET-CONSTEXPR, C23 6.7.1): the `constexpr` OBJECT storage-class.
// A constexpr object IS its translation-time value — its initializer must be a
// compile-time constant, validated AT THE DECLARATION (the empirical delta vs
// `const`, which is initializer-blind). This example exercises every supported
// arm END TO END and RUNS on all four targets:
//
//   * `constexpr int N = 5;` dimensions a REAL array TYPE — `int data[N]` with
//     `sizeof(data) == 20` proves N sized the TYPE at translation time (a
//     merely-runtime N could never reach an array bound);
//   * `static constexpr double PI2 = 3.5 * 2;` — the FLOAT-capable fold
//     (allowFloat + the float literal leaf) combined with `static` (both map
//     to internal linkage, composing idempotently);
//   * `constexpr int *NULLP = nullptr;` — the pointer arm's null-pointer-
//     constant admission (nullptr / folded integer 0 are the only accepted
//     constexpr pointer values);
//   * block-scope `constexpr char C = 'a';` + `constexpr bool B = true;` — the
//     F2 shared-evaluator leaf arms (a narrow char constant decodes via the
//     SAME decode the value path uses; true/false carry their config-declared
//     fixed values, never a text decode);
//   * the fill is ARGC-SEEDED so the array CONTENTS are runtime values — the
//     release optimizer cannot fold the sum away; only the constexpr-derived
//     terms are translation-time constants.
//
// All terms are data-model-INDEPENDENT (int is 4 bytes under LP64 and LLP64),
// so the one exit code holds across all four targets and the release pipeline.
//
// exit = sizeof(data)(20) + (int)PI2(7) + (NULLP == 0)(1) + sum(10)
//        + (C == 'a')(1) + B(1) + 2 = 42.
//   (a reverted constexpr feature fails to COMPILE — the keyword becomes an
//    unknown identifier — so this example is RED-on-revert by construction.)

constexpr int N = 5;
static constexpr double PI2 = 3.5 * 2;   // 7.0 — float fold under constexpr
constexpr int *NULLP = nullptr;          // null-pointer-constant pointer arm

int data[N];                             // N sized a TYPE: sizeof == 20

int main(int argc, char **argv) {
    (void)argv;
    constexpr char C = 'a';              // F2: narrow char constant folds
    constexpr bool B = true;             // F2: fixed-value keyword folds
    for (int i = 0; i < N; ++i) {
        data[i] = i * argc;              // runtime contents (argc-seeded)
    }
    int sum = 0;
    for (int j = 0; j < N; ++j) {
        sum = sum + data[j];             // 0+1+2+3+4 = 10 for argc == 1
    }
    int sz = (int)sizeof(data);          // 20 — N sized the TYPE
    int pi = (int)PI2;                   // 7
    int np = NULLP == 0 ? 1 : 0;         // 1
    int cc = C == 'a' ? 1 : 0;           // 1
    int bb = B ? 1 : 0;                  // 1
    return sz + pi + np + sum + cc + bb + 2;   // 20+7+1+10+1+1+2 = 42
}
